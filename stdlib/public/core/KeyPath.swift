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

import SwiftShims

internal func _abstract(
  methodName: StaticString = #function,
  file: StaticString = #file, line: UInt = #line
) -> Never {
#if INTERNAL_CHECKS_ENABLED
  _fatalErrorMessage("abstract method", methodName, file: file, line: line,
      flags: _fatalErrorFlags())
#else
  _conditionallyUnreachable()
#endif
}

// MARK: Type-erased abstract base classes

// NOTE: older runtimes had Swift.AnyKeyPath as the ObjC name.
// The two must coexist, so it was renamed. The old name must not be
// used in the new runtime. _TtCs11_AnyKeyPath is the mangled name for
// Swift._AnyKeyPath.

/// A type-erased key path, from any root type to any resulting value
/// type.
@_objcRuntimeName(_TtCs11_AnyKeyPath)
public class AnyKeyPath: Hashable, _AppendKeyPath {
  /// The root type for this key path.
  @inlinable
  public static var rootType: Any.Type {
    return _rootAndValueType.root
  }

  /// The value type for this key path.
  @inlinable
  public static var valueType: Any.Type {
    return _rootAndValueType.value
  }

  /// Used to store the offset from the root to the value
  /// in the case of a pure struct KeyPath.
  /// It's a regular kvcKeyPathStringPtr otherwise.
  internal final var _kvcKeyPathStringPtr: UnsafePointer<CChar>?
  
  /// The hash value.
  final public var hashValue: Int {
    return _hashValue(for: self)
  }

  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @_effects(releasenone)
  final public func hash(into hasher: inout Hasher) {
    ObjectIdentifier(type(of: self)).hash(into: &hasher)
    return withBuffer {
      var buffer = $0
      if buffer.data.isEmpty { return }
      while true {
        let (component, type) = buffer.next()
        hasher.combine(component.value)
        if let type = type {
          hasher.combine(unsafeBitCast(type, to: Int.self))
        } else {
          break
        }
      }
    }
  }
  
  public static func ==(a: AnyKeyPath, b: AnyKeyPath) -> Bool {
    // Fast-path identical objects
    if a === b {
      return true
    }
    // Short-circuit differently-typed key paths
    if type(of: a) != type(of: b) {
      return false
    }
    return a.withBuffer {
      var aBuffer = $0
      return b.withBuffer {
        var bBuffer = $0
        
        // Two equivalent key paths should have the same reference prefix
        if aBuffer.hasReferencePrefix != bBuffer.hasReferencePrefix {
          return false
        }
        
        // Identity is equal to identity
        if aBuffer.data.isEmpty {
          return bBuffer.data.isEmpty
        }

        while true {
          let (aComponent, aType) = aBuffer.next()
          let (bComponent, bType) = bBuffer.next()
        
          if aComponent.header.endOfReferencePrefix
              != bComponent.header.endOfReferencePrefix
            || aComponent.value != bComponent.value
            || aType != bType {
            return false
          }
          if aType == nil {
            return true
          }
        }
      }
    }
  }
    
  /*
  The following pertains to 32-bit architectures only.
  We assume everything is a valid pointer to a potential
  _kvcKeyPathStringPtr except for the first 4KB page which is reserved
  for the nil pointer. Note that we have to distinguish between a valid
  keypath offset of 0, and the nil pointer itself.
  We use maximumOffsetOn32BitArchitecture + 1 for this case.
    
  The variable maximumOffsetOn32BitArchitecture is duplicated in the two
  functions below since having it as a global would make accesses slower,
  given getOffsetFromStorage() gets called on each KeyPath read. Further,
  having it as an instance variable in AnyKeyPath would increase the size
  of AnyKeyPath by 8 bytes.
  TODO: Find a better method of refactoring this variable if possible.
  */

  func assignOffsetToStorage(offset: Int) {
    let maximumOffsetOn32BitArchitecture = 4094

    guard offset >= 0 else {
      return
    }
    // TODO: This just gets the architecture size (32 or 64 bits).
    // Is there a more efficient way? Something in Builtin maybe?
    let architectureSize = MemoryLayout<Int>.size
    if architectureSize == 8 {
      _kvcKeyPathStringPtr = UnsafePointer<CChar>(bitPattern: -offset - 1)
    }
    else {
      if offset <= maximumOffsetOn32BitArchitecture {
        _kvcKeyPathStringPtr = UnsafePointer<CChar>(bitPattern: (offset + 1))
      }
      else {
        _kvcKeyPathStringPtr = nil
      }
    }
  }

  func getOffsetFromStorage() -> Int? {
    let maximumOffsetOn32BitArchitecture = 4094
    guard _kvcKeyPathStringPtr != nil else {
      return nil
    }

    let architectureSize = MemoryLayout<Int>.size
    if architectureSize == 8 {
      let offset = -Int(bitPattern: _kvcKeyPathStringPtr) - 1
      guard offset >= 0 else {
        // This happens to be an actual _kvcKeyPathStringPtr, not an offset, if we get here.
        return nil
      }
      return offset
    }
    else {
      let offset = Int(bitPattern: _kvcKeyPathStringPtr) - 1
      // Pointers above 0x7fffffff will come in as negative numbers which are
      // less than maximumOffsetOn32BitArchitecture, be sure to reject them.
      if (offset >= 0 && offset <= maximumOffsetOn32BitArchitecture) {
        return offset
      }
      return nil
    }
  }

  // SPI for the Foundation overlay to allow interop with KVC keypath-based
  // APIs.
  public var _kvcKeyPathString: String? {
    @_semantics("keypath.kvcKeyPathString")
    get {
      guard self.getOffsetFromStorage() == nil else {
        return nil
      }
      guard let ptr = _kvcKeyPathStringPtr else { return nil }

      return String(validatingUTF8: ptr)
    }
  }
  
  // MARK: Implementation details
  
  // Prevent normal initialization. We use tail allocation via
  // allocWithTailElems().
  @available(*, unavailable)
  internal init() {
    _internalInvariantFailure("use _create(...)")
  }

  @usableFromInline
  internal class var _rootAndValueType: (root: Any.Type, value: Any.Type) {
    _abstract()
  }
  
  internal static func _create(
    capacityInBytes bytes: Int,
    initializedBy body: (UnsafeMutableRawBufferPointer) -> Void
  ) -> Self {
    _internalInvariant(bytes > 0 && bytes % 4 == 0,
                 "capacity must be multiple of 4 bytes")
    let result = Builtin.allocWithTailElems_1(self, (bytes/4)._builtinWordValue,
                                              Int32.self)
    result._kvcKeyPathStringPtr = nil
    let base = UnsafeMutableRawPointer(Builtin.projectTailElems(result,
                                                                Int32.self))
    body(UnsafeMutableRawBufferPointer(start: base, count: bytes))
    return result
  }
  
  final internal func withBuffer<T>(_ f: (KeyPathBuffer) throws -> T) rethrows -> T {
    defer { _fixLifetime(self) }
    
    let base = UnsafeRawPointer(Builtin.projectTailElems(self, Int32.self))
    return try f(KeyPathBuffer(base: base))
  }
  
  @usableFromInline // Exposed as public API by MemoryLayout<Root>.offset(of:)
  internal var _storedInlineOffset: Int? {
    return withBuffer {
      var buffer = $0

      // The identity key path is effectively a stored keypath of type Self
      // at offset zero
      if buffer.data.isEmpty { return 0 }

      var offset = 0
      while true {
        let (rawComponent, optNextType) = buffer.next()
        switch rawComponent.header.kind {
        case .struct:
          offset += rawComponent._structOrClassOffset

        case .class, .computed, .optionalChain, .optionalForce, .optionalWrap, .external:
          return .none
        }

        if optNextType == nil { return .some(offset) }
      }
    }
  }
}

/// A partially type-erased key path, from a concrete root type to any
/// resulting value type.
public class PartialKeyPath<Root>: AnyKeyPath { }

// MARK: Concrete implementations
internal enum KeyPathKind { case readOnly, value, reference }

/// A key path from a specific root type to a specific resulting value type.
///
/// The most common way to make an instance of this type
/// is by using a key-path expression like `\SomeClass.someProperty`.
/// For more information,
/// see [Key-Path Expressions][keypath] in *[The Swift Programming Language][tspl]*.
///
/// [keypath]: https://docs.swift.org/swift-book/ReferenceManual/Expressions.html#ID563
/// [tspl]: https://docs.swift.org/swift-book/
public class KeyPath<Root, Value>: PartialKeyPath<Root> {
  @usableFromInline
  internal final override class var _rootAndValueType: (
    root: Any.Type,
    value: Any.Type
  ) {
    return (Root.self, Value.self)
  }
  
  // MARK: Implementation
  internal typealias Kind = KeyPathKind
  internal class var kind: Kind { return .readOnly }
  
  internal static func appendedType<AppendedValue>(
    with t: KeyPath<Value, AppendedValue>.Type
  ) -> KeyPath<Root, AppendedValue>.Type {
    let resultKind: Kind
    switch (self.kind, t.kind) {
    case (_, .reference):
      resultKind = .reference
    case (let x, .value):
      resultKind = x
    default:
      resultKind = .readOnly
    }
    
    switch resultKind {
    case .readOnly:
      return KeyPath<Root, AppendedValue>.self
    case .value:
      return WritableKeyPath.self
    case .reference:
      return ReferenceWritableKeyPath.self
    }
  }
  
  @usableFromInline
  internal final func _projectReadOnly(from root: Root) -> Value {
      
    // One performance improvement is to skip right to Value
    // if this keypath traverses through structs only.
    if let offset = getOffsetFromStorage() {
      return withUnsafeBytes(of: root) {
        let pointer = $0.baseAddress.unsafelyUnwrapped.advanced(by: offset)
        return pointer.assumingMemoryBound(to: Value.self).pointee
      }
    }
      
    // TODO: For perf, we could use a local growable buffer instead of Any
    var curBase: Any = root
    return withBuffer {
      var buffer = $0
      if buffer.data.isEmpty {
        return unsafeBitCast(root, to: Value.self)
      }
      while true {
        let (rawComponent, optNextType) = buffer.next()
        let valueType = optNextType ?? Value.self
        let isLast = optNextType == nil
        
        func project<CurValue>(_ base: CurValue) -> Value? {
          func project2<NewValue>(_: NewValue.Type) -> Value? {
            switch rawComponent._projectReadOnly(base,
              to: NewValue.self, endingWith: Value.self) {
            case .continue(let newBase):
              if isLast {
                _internalInvariant(NewValue.self == Value.self,
                             "key path does not terminate in correct type")
                return unsafeBitCast(newBase, to: Value.self)
              } else {
                curBase = newBase
                return nil
              }
            case .break(let result):
              return result
            }
          }

          return _openExistential(valueType, do: project2)
        }

        if let result = _openExistential(curBase, do: project) {
          return result
        }
      }
    }
  }
  
  deinit {
    withBuffer { $0.destroy() }
  }
}

/// A key path that supports reading from and writing to the resulting value.
public class WritableKeyPath<Root, Value>: KeyPath<Root, Value> {
  // MARK: Implementation detail
  
  internal override class var kind: Kind { return .value }

  // `base` is assumed to be undergoing a formal access for the duration of the
  // call, so must not be mutated by an alias
  @usableFromInline
  internal func _projectMutableAddress(from base: UnsafePointer<Root>)
      -> (pointer: UnsafeMutablePointer<Value>, owner: AnyObject?) {
   
    // One performance improvement is to skip right to Value
    // if this keypath traverses through structs only.
          
    // Don't declare "p" above this if-statement; it may slow things down.
    if let offset = getOffsetFromStorage()
    {
      let p = UnsafeRawPointer(base).advanced(by: offset)
      return (pointer: UnsafeMutablePointer(
        mutating: p.assumingMemoryBound(to: Value.self)), owner: nil)
    }
    var p = UnsafeRawPointer(base)
    var type: Any.Type = Root.self
    var keepAlive: AnyObject?
    
    return withBuffer {
      var buffer = $0
      
      _internalInvariant(!buffer.hasReferencePrefix,
                   "WritableKeyPath should not have a reference prefix")
      
      if buffer.data.isEmpty {
        return (
          UnsafeMutablePointer<Value>(
            mutating: p.assumingMemoryBound(to: Value.self)),
          nil)
      }

      while true {
        let (rawComponent, optNextType) = buffer.next()
        let nextType = optNextType ?? Value.self
        
        func project<CurValue>(_: CurValue.Type) {
          func project2<NewValue>(_: NewValue.Type) {
            p = rawComponent._projectMutableAddress(p,
                                           from: CurValue.self,
                                           to: NewValue.self,
                                           isRoot: p == UnsafeRawPointer(base),
                                           keepAlive: &keepAlive)
          }
          _openExistential(nextType, do: project2)
        }
        _openExistential(type, do: project)
        
        if optNextType == nil { break }
        type = nextType
      }
      // TODO: With coroutines, it would be better to yield here, so that
      // we don't need the hack of the keepAlive reference to manage closing
      // accesses.
      let typedPointer = p.assumingMemoryBound(to: Value.self)
      return (pointer: UnsafeMutablePointer(mutating: typedPointer),
              owner: keepAlive)
    }
  }
}

/// A key path that supports reading from and writing to the resulting value
/// with reference semantics.
public class ReferenceWritableKeyPath<
  Root, Value
>: WritableKeyPath<Root, Value> {
  // MARK: Implementation detail

  internal final override class var kind: Kind { return .reference }
  
  @usableFromInline
  internal final func _projectMutableAddress(from origBase: Root)
      -> (pointer: UnsafeMutablePointer<Value>, owner: AnyObject?) {
    var keepAlive: AnyObject?
    let address: UnsafeMutablePointer<Value> = withBuffer {
      var buffer = $0
      // Project out the reference prefix.
      var base: Any = origBase
      while buffer.hasReferencePrefix {
        let (rawComponent, optNextType) = buffer.next()
        _internalInvariant(optNextType != nil,
                     "reference prefix should not go to end of buffer")
        let nextType = optNextType.unsafelyUnwrapped
        
        func project<NewValue>(_: NewValue.Type) -> Any {
          func project2<CurValue>(_ base: CurValue) -> Any {
            return rawComponent._projectReadOnly(
              base, to: NewValue.self, endingWith: Value.self)
              .assumingContinue
          }
          return _openExistential(base, do: project2)
        }
        base = _openExistential(nextType, do: project)
      }
      
      // Start formal access to the mutable value, based on the final base
      // value.
      func formalMutation<MutationRoot>(_ base: MutationRoot)
          -> UnsafeMutablePointer<Value> {
        var base2 = base
        return withUnsafeBytes(of: &base2) { baseBytes in
          var p = baseBytes.baseAddress.unsafelyUnwrapped
          var curType: Any.Type = MutationRoot.self
          while true {
            let (rawComponent, optNextType) = buffer.next()
            let nextType = optNextType ?? Value.self
            func project<CurValue>(_: CurValue.Type) {
              func project2<NewValue>(_: NewValue.Type) {
                p = rawComponent._projectMutableAddress(p,
                                             from: CurValue.self,
                                             to: NewValue.self,
                                             isRoot: p == baseBytes.baseAddress,
                                             keepAlive: &keepAlive)
              }
              _openExistential(nextType, do: project2)
            }
            _openExistential(curType, do: project)

            if optNextType == nil { break }
            curType = nextType
          }
          let typedPointer = p.assumingMemoryBound(to: Value.self)
          return UnsafeMutablePointer(mutating: typedPointer)
        }
      }
      return _openExistential(base, do: formalMutation)
    }
    
    return (address, keepAlive)
  }
}

// MARK: Implementation details

internal enum KeyPathComponentKind {
  /// The keypath references an externally-defined property or subscript whose
  /// component describes how to interact with the key path.
  case external
  /// The keypath projects within the storage of the outer value, like a
  /// stored property in a struct.
  case `struct`
  /// The keypath projects from the referenced pointer, like a
  /// stored property in a class.
  case `class`
  /// The keypath projects using a getter/setter pair.
  case computed
  /// The keypath optional-chains, returning nil immediately if the input is
  /// nil, or else proceeding by projecting the value inside.
  case optionalChain
  /// The keypath optional-forces, trapping if the input is
  /// nil, or else proceeding by projecting the value inside.
  case optionalForce
  /// The keypath wraps a value in an optional.
  case optionalWrap
}

internal struct ComputedPropertyID: Hashable {
  internal var value: Int
  internal var kind: KeyPathComputedIDKind

  internal static func ==(
    x: ComputedPropertyID, y: ComputedPropertyID
  ) -> Bool {
    return x.value == y.value
      && x.kind == y.kind
  }

  internal func hash(into hasher: inout Hasher) {
    hasher.combine(value)
    hasher.combine(kind)
  }
}

internal struct ComputedAccessorsPtr {
#if INTERNAL_CHECKS_ENABLED
  internal let header: RawKeyPathComponent.Header
#endif
  internal let _value: UnsafeRawPointer

  init(header: RawKeyPathComponent.Header, value: UnsafeRawPointer) {
#if INTERNAL_CHECKS_ENABLED
    self.header = header
#endif
    self._value = value
  }

  @_transparent
  static var getterPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_Getter)
  }
  @_transparent
  static var nonmutatingSetterPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_NonmutatingSetter)
  }
  @_transparent
  static var mutatingSetterPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_MutatingSetter)
  }

  internal typealias Getter<CurValue, NewValue> = @convention(thin)
    (CurValue, UnsafeRawPointer, Int) -> NewValue
  internal typealias NonmutatingSetter<CurValue, NewValue> = @convention(thin)
    (NewValue, CurValue, UnsafeRawPointer, Int) -> ()
  internal typealias MutatingSetter<CurValue, NewValue> = @convention(thin)
    (NewValue, inout CurValue, UnsafeRawPointer, Int) -> ()

  internal var getterPtr: UnsafeRawPointer {
#if INTERNAL_CHECKS_ENABLED
    _internalInvariant(header.kind == .computed,
                 "not a computed property")
#endif
    return _value
  }
  internal var setterPtr: UnsafeRawPointer {
#if INTERNAL_CHECKS_ENABLED
    _internalInvariant(header.isComputedSettable,
                 "not a settable property")
#endif
    return _value + MemoryLayout<Int>.size
  }

  internal func getter<CurValue, NewValue>()
      -> Getter<CurValue, NewValue> {

    return getterPtr._loadAddressDiscriminatedFunctionPointer(
      as: Getter.self,
      discriminator: ComputedAccessorsPtr.getterPtrAuthKey)
  }

  internal func nonmutatingSetter<CurValue, NewValue>()
      -> NonmutatingSetter<CurValue, NewValue> {
#if INTERNAL_CHECKS_ENABLED
    _internalInvariant(header.isComputedSettable && !header.isComputedMutating,
                 "not a nonmutating settable property")
#endif

    return setterPtr._loadAddressDiscriminatedFunctionPointer(
      as: NonmutatingSetter.self,
      discriminator: ComputedAccessorsPtr.nonmutatingSetterPtrAuthKey)
  }

  internal func mutatingSetter<CurValue, NewValue>()
      -> MutatingSetter<CurValue, NewValue> {
#if INTERNAL_CHECKS_ENABLED
    _internalInvariant(header.isComputedSettable && header.isComputedMutating,
                 "not a mutating settable property")
#endif

    return setterPtr._loadAddressDiscriminatedFunctionPointer(
      as: MutatingSetter.self,
      discriminator: ComputedAccessorsPtr.mutatingSetterPtrAuthKey)
  }
}

internal struct ComputedArgumentWitnessesPtr {
  internal let _value: UnsafeRawPointer

  init(_ value: UnsafeRawPointer) {
    self._value = value
  }

  @_transparent
  static var destroyPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentDestroy)
  }
  @_transparent
  static var copyPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentCopy)
  }
  @_transparent
  static var equalsPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentEquals)
  }
  @_transparent
  static var hashPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentHash)
  }
  @_transparent
  static var layoutPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentLayout)
  }
  @_transparent
  static var initPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_ArgumentInit)
  }

  internal typealias Destroy = @convention(thin)
    (_ instanceArguments: UnsafeMutableRawPointer, _ size: Int) -> ()
  internal typealias Copy = @convention(thin)
    (_ srcInstanceArguments: UnsafeRawPointer,
     _ destInstanceArguments: UnsafeMutableRawPointer,
     _ size: Int) -> ()
  internal typealias Equals = @convention(thin)
    (_ xInstanceArguments: UnsafeRawPointer,
     _ yInstanceArguments: UnsafeRawPointer,
     _ size: Int) -> Bool
  // FIXME(hasher) Combine to an inout Hasher instead
  internal typealias Hash = @convention(thin)
    (_ instanceArguments: UnsafeRawPointer,
     _ size: Int) -> Int

  // The witnesses are stored as address-discriminated authenticated
  // pointers.

  internal var destroy: Destroy? {
    return _value._loadAddressDiscriminatedFunctionPointer(
      as: Optional<Destroy>.self,
      discriminator: ComputedArgumentWitnessesPtr.destroyPtrAuthKey)
  }
  internal var copy: Copy {
    return _value._loadAddressDiscriminatedFunctionPointer(
      fromByteOffset: MemoryLayout<UnsafeRawPointer>.size,
      as: Copy.self,
      discriminator: ComputedArgumentWitnessesPtr.copyPtrAuthKey)
  }
  internal var equals: Equals {
    return _value._loadAddressDiscriminatedFunctionPointer(
      fromByteOffset: 2*MemoryLayout<UnsafeRawPointer>.size,
      as: Equals.self,
      discriminator: ComputedArgumentWitnessesPtr.equalsPtrAuthKey)
  }
  internal var hash: Hash {
    return _value._loadAddressDiscriminatedFunctionPointer(
      fromByteOffset: 3*MemoryLayout<UnsafeRawPointer>.size,
      as: Hash.self,
      discriminator: ComputedArgumentWitnessesPtr.hashPtrAuthKey)
  }
}

internal enum KeyPathComponent: Hashable {
  internal struct ArgumentRef {
    internal var data: UnsafeRawBufferPointer
    internal var witnesses: ComputedArgumentWitnessesPtr
    internal var witnessSizeAdjustment: Int

    internal init(
      data: UnsafeRawBufferPointer,
      witnesses: ComputedArgumentWitnessesPtr,
      witnessSizeAdjustment: Int
    ) {
      self.data = data
      self.witnesses = witnesses
      self.witnessSizeAdjustment = witnessSizeAdjustment
    }
  }

  /// The keypath projects within the storage of the outer value, like a
  /// stored property in a struct.
  case `struct`(offset: Int)
  /// The keypath projects from the referenced pointer, like a
  /// stored property in a class.
  case `class`(offset: Int)
  /// The keypath projects using a getter.
  case get(id: ComputedPropertyID,
           accessors: ComputedAccessorsPtr,
           argument: ArgumentRef?)
  /// The keypath projects using a getter/setter pair. The setter can mutate
  /// the base value in-place.
  case mutatingGetSet(id: ComputedPropertyID,
                      accessors: ComputedAccessorsPtr,
                      argument: ArgumentRef?)
  /// The keypath projects using a getter/setter pair that does not mutate its
  /// base.
  case nonmutatingGetSet(id: ComputedPropertyID,
                         accessors: ComputedAccessorsPtr,
                         argument: ArgumentRef?)
  /// The keypath optional-chains, returning nil immediately if the input is
  /// nil, or else proceeding by projecting the value inside.
  case optionalChain
  /// The keypath optional-forces, trapping if the input is
  /// nil, or else proceeding by projecting the value inside.
  case optionalForce
  /// The keypath wraps a value in an optional.
  case optionalWrap

  internal static func ==(a: KeyPathComponent, b: KeyPathComponent) -> Bool {
    switch (a, b) {
    case (.struct(offset: let a), .struct(offset: let b)),
         (.class (offset: let a), .class (offset: let b)):
      return a == b
    case (.optionalChain, .optionalChain),
         (.optionalForce, .optionalForce),
         (.optionalWrap, .optionalWrap):
      return true
    case (.get(id: let id1, accessors: _, argument: let argument1),
          .get(id: let id2, accessors: _, argument: let argument2)),

         (.mutatingGetSet(id: let id1, accessors: _, argument: let argument1),
          .mutatingGetSet(id: let id2, accessors: _, argument: let argument2)),

         (.nonmutatingGetSet(id: let id1, accessors: _, argument: let argument1),
          .nonmutatingGetSet(id: let id2, accessors: _, argument: let argument2)):
      if id1 != id2 {
        return false
      }
      if let arg1 = argument1, let arg2 = argument2 {
        return arg1.witnesses.equals(
          arg1.data.baseAddress.unsafelyUnwrapped,
          arg2.data.baseAddress.unsafelyUnwrapped,
          arg1.data.count - arg1.witnessSizeAdjustment)
      }
      // If only one component has arguments, that should indicate that the
      // only arguments in that component were generic captures and therefore
      // not affecting equality.
      return true
    case (.struct, _),
         (.class,  _),
         (.optionalChain, _),
         (.optionalForce, _),
         (.optionalWrap, _),
         (.get, _),
         (.mutatingGetSet, _),
         (.nonmutatingGetSet, _):
      return false
    }
  }

  @_effects(releasenone)
  internal func hash(into hasher: inout Hasher) {
    func appendHashFromArgument(
      _ argument: KeyPathComponent.ArgumentRef?
    ) {
      if let argument = argument {
        let hash = argument.witnesses.hash(
          argument.data.baseAddress.unsafelyUnwrapped,
          argument.data.count - argument.witnessSizeAdjustment)
        // Returning 0 indicates that the arguments should not impact the
        // hash value of the overall key path.
        // FIXME(hasher): hash witness should just mutate hasher directly
        if hash != 0 {
          hasher.combine(hash)
        }
      }
    }
    switch self {
    case .struct(offset: let a):
      hasher.combine(0)
      hasher.combine(a)
    case .class(offset: let b):
      hasher.combine(1)
      hasher.combine(b)
    case .optionalChain:
      hasher.combine(2)
    case .optionalForce:
      hasher.combine(3)
    case .optionalWrap:
      hasher.combine(4)
    case .get(id: let id, accessors: _, argument: let argument):
      hasher.combine(5)
      hasher.combine(id)
      appendHashFromArgument(argument)
    case .mutatingGetSet(id: let id, accessors: _, argument: let argument):
      hasher.combine(6)
      hasher.combine(id)
      appendHashFromArgument(argument)
    case .nonmutatingGetSet(id: let id, accessors: _, argument: let argument):
      hasher.combine(7)
      hasher.combine(id)
      appendHashFromArgument(argument)
    }
  }
}

// A class that maintains ownership of another object while a mutable projection
// into it is underway. The lifetime of the instance of this class is also used
// to begin and end exclusive 'modify' access to the projected address.
internal final class ClassHolder<ProjectionType> {

  /// The type of the scratch record passed to the runtime to record
  /// accesses to guarantee exclusive access.
  internal typealias AccessRecord = Builtin.UnsafeValueBuffer

  internal var previous: AnyObject?
  internal var instance: AnyObject

  internal init(previous: AnyObject?, instance: AnyObject) {
    self.previous = previous
    self.instance = instance
  }

  internal final class func _create(
      previous: AnyObject?,
      instance: AnyObject,
      accessingAddress address: UnsafeRawPointer,
      type: ProjectionType.Type
  ) -> ClassHolder {

    // Tail allocate the UnsafeValueBuffer used as the AccessRecord.
    // This avoids a second heap allocation since there is no source-level way to
    // initialize a Builtin.UnsafeValueBuffer type and thus we cannot have a
    // stored property of that type.
    let holder: ClassHolder = Builtin.allocWithTailElems_1(self,
                                                          1._builtinWordValue,
                                                          AccessRecord.self)

    // Initialize the ClassHolder's instance variables. This is done via
    // withUnsafeMutablePointer(to:) because the instance was just allocated with
    // allocWithTailElems_1 and so we need to make sure to use an initialization
    // rather than an assignment.
    withUnsafeMutablePointer(to: &holder.previous) {
      $0.initialize(to: previous)
    }

    withUnsafeMutablePointer(to: &holder.instance) {
      $0.initialize(to: instance)
    }

    let accessRecordPtr = Builtin.projectTailElems(holder, AccessRecord.self)

    // Begin a 'modify' access to the address. This access is ended in
    // ClassHolder's deinitializer.
    Builtin.beginUnpairedModifyAccess(address._rawValue, accessRecordPtr, type)

    return holder
  }

  deinit {
    let accessRecordPtr = Builtin.projectTailElems(self, AccessRecord.self)

    // Ends the access begun in _create().
    Builtin.endUnpairedAccess(accessRecordPtr)
  }
}

// A class that triggers writeback to a pointer when destroyed.
internal final class MutatingWritebackBuffer<CurValue, NewValue> {
  internal let previous: AnyObject?
  internal let base: UnsafeMutablePointer<CurValue>
  internal let set: ComputedAccessorsPtr.MutatingSetter<CurValue, NewValue>
  internal let argument: UnsafeRawPointer
  internal let argumentSize: Int
  internal var value: NewValue

  deinit {
    set(value, &base.pointee, argument, argumentSize)
  }

  internal init(previous: AnyObject?,
       base: UnsafeMutablePointer<CurValue>,
       set: @escaping ComputedAccessorsPtr.MutatingSetter<CurValue, NewValue>,
       argument: UnsafeRawPointer,
       argumentSize: Int,
       value: NewValue) {
    self.previous = previous
    self.base = base
    self.set = set
    self.argument = argument
    self.argumentSize = argumentSize
    self.value = value
  }
}

// A class that triggers writeback to a non-mutated value when destroyed.
internal final class NonmutatingWritebackBuffer<CurValue, NewValue> {
  internal let previous: AnyObject?
  internal let base: CurValue
  internal let set: ComputedAccessorsPtr.NonmutatingSetter<CurValue, NewValue>
  internal let argument: UnsafeRawPointer
  internal let argumentSize: Int
  internal var value: NewValue

  deinit {
    set(value, base, argument, argumentSize)
  }

  internal
  init(previous: AnyObject?,
       base: CurValue,
       set: @escaping ComputedAccessorsPtr.NonmutatingSetter<CurValue, NewValue>,
       argument: UnsafeRawPointer,
       argumentSize: Int,
       value: NewValue) {
    self.previous = previous
    self.base = base
    self.set = set
    self.argument = argument
    self.argumentSize = argumentSize
    self.value = value
  }
}

internal typealias KeyPathComputedArgumentLayoutFn = @convention(thin)
  (_ patternArguments: UnsafeRawPointer?) -> (size: Int, alignmentMask: Int)
internal typealias KeyPathComputedArgumentInitializerFn = @convention(thin)
  (_ patternArguments: UnsafeRawPointer?,
   _ instanceArguments: UnsafeMutableRawPointer) -> ()

internal enum KeyPathComputedIDKind {
  case pointer
  case storedPropertyIndex
  case vtableOffset
}

internal enum KeyPathComputedIDResolution {
  case resolved
  case resolvedAbsolute
  case indirectPointer
  case functionCall
}

internal struct RawKeyPathComponent {
  internal var header: Header
  internal var body: UnsafeRawBufferPointer

  internal init(header: Header, body: UnsafeRawBufferPointer) {
    self.header = header
    self.body = body
  }

  @_transparent
  static var metadataAccessorPtrAuthKey: UInt64 {
    return UInt64(_SwiftKeyPath_ptrauth_MetadataAccessor)
  }

  internal struct Header {
    internal var _value: UInt32

    init(discriminator: UInt32, payload: UInt32) {
      _value = 0
      self.discriminator = discriminator
      self.payload = payload
    }

    internal var discriminator: UInt32 {
      get {
        return (_value & Header.discriminatorMask) >> Header.discriminatorShift
      }
      set {
        let shifted = newValue << Header.discriminatorShift
        _internalInvariant(shifted & Header.discriminatorMask == shifted,
                     "discriminator doesn't fit")
        _value = _value & ~Header.discriminatorMask | shifted
      }
    }
    internal var payload: UInt32 {
      get {
        return _value & Header.payloadMask
      }
      set {
        _internalInvariant(newValue & Header.payloadMask == newValue,
                     "payload too big")
        _value = _value & ~Header.payloadMask | newValue
      }
    }
    internal var storedOffsetPayload: UInt32 {
      get {
        _internalInvariant(kind == .struct || kind == .class,
                     "not a stored component")
        return _value & Header.storedOffsetPayloadMask
      }
      set {
        _internalInvariant(kind == .struct || kind == .class,
                     "not a stored component")
        _internalInvariant(newValue & Header.storedOffsetPayloadMask == newValue,
                     "payload too big")
        _value = _value & ~Header.storedOffsetPayloadMask | newValue
      }
    }
    internal var endOfReferencePrefix: Bool {
      get {
        return _value & Header.endOfReferencePrefixFlag != 0
      }
      set {
        if newValue {
          _value |= Header.endOfReferencePrefixFlag
        } else {
          _value &= ~Header.endOfReferencePrefixFlag
        }
      }
    }

    internal var kind: KeyPathComponentKind {
      switch (discriminator, payload) {
      case (Header.externalTag, _):
        return .external
      case (Header.structTag, _):
        return .struct
      case (Header.classTag, _):
        return .class
      case (Header.computedTag, _):
        return .computed
      case (Header.optionalTag, Header.optionalChainPayload):
        return .optionalChain
      case (Header.optionalTag, Header.optionalWrapPayload):
        return .optionalWrap
      case (Header.optionalTag, Header.optionalForcePayload):
        return .optionalForce
      default:
        _internalInvariantFailure("invalid header")
      }
    }

    internal static var payloadMask: UInt32 {
      return _SwiftKeyPathComponentHeader_PayloadMask
    }
    internal static var discriminatorMask: UInt32 {
      return _SwiftKeyPathComponentHeader_DiscriminatorMask
    }
    internal static var discriminatorShift: UInt32 {
      return _SwiftKeyPathComponentHeader_DiscriminatorShift
    }
    internal static var externalTag: UInt32 {
      return _SwiftKeyPathComponentHeader_ExternalTag
    }
    internal static var structTag: UInt32 {
      return _SwiftKeyPathComponentHeader_StructTag
    }
    internal static var computedTag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedTag
    }
    internal static var classTag: UInt32 {
      return _SwiftKeyPathComponentHeader_ClassTag
    }
    internal static var optionalTag: UInt32 {
      return _SwiftKeyPathComponentHeader_OptionalTag
    }
    internal static var optionalChainPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_OptionalChainPayload
    }
    internal static var optionalWrapPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_OptionalWrapPayload
    }
    internal static var optionalForcePayload: UInt32 {
      return _SwiftKeyPathComponentHeader_OptionalForcePayload
    }

    internal static var endOfReferencePrefixFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_EndOfReferencePrefixFlag
    }
    internal static var storedMutableFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_StoredMutableFlag
    }
    internal static var storedOffsetPayloadMask: UInt32 {
      return _SwiftKeyPathComponentHeader_StoredOffsetPayloadMask
    }
    internal static var outOfLineOffsetPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_OutOfLineOffsetPayload
    }
    internal static var unresolvedFieldOffsetPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_UnresolvedFieldOffsetPayload
    }
    internal static var unresolvedIndirectOffsetPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_UnresolvedIndirectOffsetPayload
    }
    internal static var maximumOffsetPayload: UInt32 {
      return _SwiftKeyPathComponentHeader_MaximumOffsetPayload
    }

    internal var isStoredMutable: Bool {
      _internalInvariant(kind == .struct || kind == .class)
      return _value & Header.storedMutableFlag != 0
    }

    internal static var computedMutatingFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedMutatingFlag
    }
    internal var isComputedMutating: Bool {
      _internalInvariant(kind == .computed)
      return _value & Header.computedMutatingFlag != 0
    }

    internal static var computedSettableFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedSettableFlag
    }
    internal var isComputedSettable: Bool {
      _internalInvariant(kind == .computed)
      return _value & Header.computedSettableFlag != 0
    }

    internal static var computedIDByStoredPropertyFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDByStoredPropertyFlag
    }
    internal static var computedIDByVTableOffsetFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDByVTableOffsetFlag
    }
    internal var computedIDKind: KeyPathComputedIDKind {
      let storedProperty = _value & Header.computedIDByStoredPropertyFlag != 0
      let vtableOffset = _value & Header.computedIDByVTableOffsetFlag != 0

      switch (storedProperty, vtableOffset) {
      case (true, true):
        _internalInvariantFailure("not allowed")
      case (true, false):
        return .storedPropertyIndex
      case (false, true):
        return .vtableOffset
      case (false, false):
        return .pointer
      }
    }

    internal static var computedHasArgumentsFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedHasArgumentsFlag
    }
    internal var hasComputedArguments: Bool {
      _internalInvariant(kind == .computed)
      return _value & Header.computedHasArgumentsFlag != 0
    }

    // If a computed component is instantiated from an external property
    // descriptor, and both components carry arguments, we need to carry some
    // extra matter to be able to map between the client and external generic
    // contexts.
    internal static var computedInstantiatedFromExternalWithArgumentsFlag: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedInstantiatedFromExternalWithArgumentsFlag
    }
    internal var isComputedInstantiatedFromExternalWithArguments: Bool {
      get {
        _internalInvariant(kind == .computed)
        return
          _value & Header.computedInstantiatedFromExternalWithArgumentsFlag != 0
      }
      set {
        _internalInvariant(kind == .computed)
        _value =
            _value & ~Header.computedInstantiatedFromExternalWithArgumentsFlag
          | (newValue ? Header.computedInstantiatedFromExternalWithArgumentsFlag
                      : 0)
      }
    }
    internal static var externalWithArgumentsExtraSize: Int {
      return MemoryLayout<Int>.size
    }

    internal static var computedIDResolutionMask: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDResolutionMask
    }
    internal static var computedIDResolved: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDResolved
    }
    internal static var computedIDResolvedAbsolute: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDResolvedAbsolute
    }
    internal static var computedIDUnresolvedIndirectPointer: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDUnresolvedIndirectPointer
    }
    internal static var computedIDUnresolvedFunctionCall: UInt32 {
      return _SwiftKeyPathComponentHeader_ComputedIDUnresolvedFunctionCall
    }
    internal var computedIDResolution: KeyPathComputedIDResolution {
      switch payload & Header.computedIDResolutionMask {
      case Header.computedIDResolved:
        return .resolved
      case Header.computedIDResolvedAbsolute:
        return .resolvedAbsolute
      case Header.computedIDUnresolvedIndirectPointer:
        return .indirectPointer
      case Header.computedIDUnresolvedFunctionCall:
        return .functionCall
      default:
        _internalInvariantFailure("invalid key path resolution")
      }
    }

    // The component header is 4 bytes, but may be followed by an aligned
    // pointer field for some kinds of component, forcing padding.
    internal static var pointerAlignmentSkew: Int {
      return MemoryLayout<Int>.size - MemoryLayout<Int32>.size
    }

    internal var isTrivialPropertyDescriptor: Bool {
      return _value ==
        _SwiftKeyPathComponentHeader_TrivialPropertyDescriptorMarker
    }

    /// If this is the header for a component in a key path pattern, return
    /// the size of the body of the component.
    internal var patternComponentBodySize: Int {
      return _componentBodySize(forPropertyDescriptor: false)
    }

    /// If this is the header for a property descriptor, return
    /// the size of the body of the component.
    internal var propertyDescriptorBodySize: Int {
      if isTrivialPropertyDescriptor { return 0 }
      return _componentBodySize(forPropertyDescriptor: true)
    }

    internal func _componentBodySize(forPropertyDescriptor: Bool) -> Int {
      switch kind {
      case .struct, .class:
        if storedOffsetPayload == Header.unresolvedFieldOffsetPayload
           || storedOffsetPayload == Header.outOfLineOffsetPayload
           || storedOffsetPayload == Header.unresolvedIndirectOffsetPayload {
          // A 32-bit offset is stored in the body.
          return MemoryLayout<UInt32>.size
        }
        // Otherwise, there's no body.
        return 0

      case .external:
        // The body holds a pointer to the external property descriptor,
        // and some number of substitution arguments, the count of which is
        // in the payload.
        return 4 * (1 + Int(payload))

      case .computed:
        // The body holds at minimum the id and getter.
        var size = 8
        // If settable, it also holds the setter.
        if isComputedSettable {
          size += 4
        }
        // If there are arguments, there's also a layout function,
        // witness table, and initializer function.
        // Property descriptors never carry argument information, though.
        if !forPropertyDescriptor && hasComputedArguments {
          size += 12
        }

        return size

      case .optionalForce, .optionalChain, .optionalWrap:
        // Otherwise, there's no body.
        return 0
      }
    }

    init(optionalForce: ()) {
      self.init(discriminator: Header.optionalTag,
                payload: Header.optionalForcePayload)
    }

    init(optionalWrap: ()) {
      self.init(discriminator: Header.optionalTag,
                payload: Header.optionalWrapPayload)
    }

    init(optionalChain: ()) {
      self.init(discriminator: Header.optionalTag,
                payload: Header.optionalChainPayload)
    }

    init(stored kind: KeyPathStructOrClass,
         mutable: Bool,
         inlineOffset: UInt32) {
      let discriminator: UInt32
      switch kind {
      case .struct: discriminator = Header.structTag
      case .class: discriminator = Header.classTag
      }

      _internalInvariant(inlineOffset <= Header.maximumOffsetPayload)
      let payload = inlineOffset
        | (mutable ? Header.storedMutableFlag : 0)
      self.init(discriminator: discriminator,
                payload: payload)
    }

    init(storedWithOutOfLineOffset kind: KeyPathStructOrClass,
         mutable: Bool) {
      let discriminator: UInt32
      switch kind {
      case .struct: discriminator = Header.structTag
      case .class: discriminator = Header.classTag
      }

      let payload = Header.outOfLineOffsetPayload
        | (mutable ? Header.storedMutableFlag : 0)

      self.init(discriminator: discriminator,
                payload: payload)
    }

    init(computedWithIDKind kind: KeyPathComputedIDKind,
         mutating: Bool,
         settable: Bool,
         hasArguments: Bool,
         instantiatedFromExternalWithArguments: Bool) {
      let discriminator = Header.computedTag
      var payload =
          (mutating ? Header.computedMutatingFlag : 0)
        | (settable ? Header.computedSettableFlag : 0)
        | (hasArguments ? Header.computedHasArgumentsFlag : 0)
        | (instantiatedFromExternalWithArguments
             ? Header.computedInstantiatedFromExternalWithArgumentsFlag : 0)
      switch kind {
      case .pointer:
        break
      case .storedPropertyIndex:
        payload |= Header.computedIDByStoredPropertyFlag
      case .vtableOffset:
        payload |= Header.computedIDByVTableOffsetFlag
      }
      self.init(discriminator: discriminator,
                payload: payload)
    }
  }

  internal var bodySize: Int {
    let ptrSize = MemoryLayout<Int>.size
    switch header.kind {
    case .struct, .class:
      if header.storedOffsetPayload == Header.outOfLineOffsetPayload {
        return 4 // overflowed
      }
      return 0
    case .external:
      _internalInvariantFailure("should be instantiated away")
    case .optionalChain, .optionalForce, .optionalWrap:
      return 0
    case .computed:
      // align to pointer, minimum two pointers for id and get
      var total = Header.pointerAlignmentSkew + ptrSize * 2
      // additional word for a setter
      if header.isComputedSettable {
        total += ptrSize
      }
      // include the argument size
      if header.hasComputedArguments {
        // two words for argument header: size, witnesses
        total += ptrSize * 2
        // size of argument area
        total += _computedArgumentSize
        if header.isComputedInstantiatedFromExternalWithArguments {
          total += Header.externalWithArgumentsExtraSize
        }
      }
      return total
    }
  }

  internal var _structOrClassOffset: Int {
    _internalInvariant(header.kind == .struct || header.kind == .class,
                 "no offset for this kind")
    // An offset too large to fit inline is represented by a signal and stored
    // in the body.
    if header.storedOffsetPayload == Header.outOfLineOffsetPayload {
      // Offset overflowed into body
      _internalInvariant(body.count >= MemoryLayout<UInt32>.size,
                   "component not big enough")
      return Int(body.load(as: UInt32.self))
    }
    return Int(header.storedOffsetPayload)
  }

  internal var _computedIDValue: Int {
    _internalInvariant(header.kind == .computed,
                 "not a computed property")
    return body.load(fromByteOffset: Header.pointerAlignmentSkew,
                     as: Int.self)
  }

  internal var _computedID: ComputedPropertyID {
    _internalInvariant(header.kind == .computed,
                 "not a computed property")

    return ComputedPropertyID(
      value: _computedIDValue,
      kind: header.computedIDKind)
  }

  internal var _computedAccessors: ComputedAccessorsPtr {
    _internalInvariant(header.kind == .computed,
                 "not a computed property")

    return ComputedAccessorsPtr(
      header: header,
      value: body.baseAddress.unsafelyUnwrapped +
              Header.pointerAlignmentSkew + MemoryLayout<Int>.size)
  }

  internal var _computedArgumentHeaderPointer: UnsafeRawPointer {
    _internalInvariant(header.hasComputedArguments, "no arguments")

    return body.baseAddress.unsafelyUnwrapped
      + Header.pointerAlignmentSkew
      + MemoryLayout<Int>.size *
         (header.isComputedSettable ? 3 : 2)
  }

  internal var _computedArgumentSize: Int {
    return _computedArgumentHeaderPointer.load(as: Int.self)
  }
  internal
  var _computedArgumentWitnesses: ComputedArgumentWitnessesPtr {
    return _computedArgumentHeaderPointer.load(
      fromByteOffset: MemoryLayout<Int>.size,
      as: ComputedArgumentWitnessesPtr.self)
  }

  internal var _computedArguments: UnsafeRawPointer {
    var base = _computedArgumentHeaderPointer + MemoryLayout<Int>.size * 2
    // If the component was instantiated from an external property descriptor
    // with its own arguments, we include some additional capture info to
    // be able to map to the original argument context by adjusting the size
    // passed to the witness operations.
    if header.isComputedInstantiatedFromExternalWithArguments {
      base += Header.externalWithArgumentsExtraSize
    }
    return base
  }
  internal var _computedMutableArguments: UnsafeMutableRawPointer {
    return UnsafeMutableRawPointer(mutating: _computedArguments)
  }
  internal var _computedArgumentWitnessSizeAdjustment: Int {
    if header.isComputedInstantiatedFromExternalWithArguments {
      return _computedArguments.load(
        fromByteOffset: -Header.externalWithArgumentsExtraSize,
        as: Int.self)
    }
    return 0
  }

  internal var value: KeyPathComponent {
    switch header.kind {
    case .struct:
      return .struct(offset: _structOrClassOffset)
    case .class:
      return .class(offset: _structOrClassOffset)
    case .optionalChain:
      return .optionalChain
    case .optionalForce:
      return .optionalForce
    case .optionalWrap:
      return .optionalWrap
    case .computed:
      let isSettable = header.isComputedSettable
      let isMutating = header.isComputedMutating

      let id = _computedID
      let accessors = _computedAccessors
      // Argument value is unused if there are no arguments.
      let argument: KeyPathComponent.ArgumentRef?
      if header.hasComputedArguments {
        argument = KeyPathComponent.ArgumentRef(
          data: UnsafeRawBufferPointer(start: _computedArguments,
                                       count: _computedArgumentSize),
          witnesses: _computedArgumentWitnesses,
          witnessSizeAdjustment: _computedArgumentWitnessSizeAdjustment)
      } else {
        argument = nil
      }

      switch (isSettable, isMutating) {
      case (false, false):
        return .get(id: id, accessors: accessors, argument: argument)
      case (true, false):
        return .nonmutatingGetSet(id: id,
                                  accessors: accessors,
                                  argument: argument)
      case (true, true):
        return .mutatingGetSet(id: id,
                               accessors: accessors,
                               argument: argument)
      case (false, true):
        _internalInvariantFailure("impossible")
      }
    case .external:
      _internalInvariantFailure("should have been instantiated away")
    }
  }

  internal func destroy() {
    switch header.kind {
    case .struct,
         .class,
         .optionalChain,
         .optionalForce,
         .optionalWrap:
      // trivial
      break
    case .computed:
      // Run destructor, if any
      if header.hasComputedArguments,
         let destructor = _computedArgumentWitnesses.destroy {
        destructor(_computedMutableArguments,
                 _computedArgumentSize - _computedArgumentWitnessSizeAdjustment)
      }
    case .external:
      _internalInvariantFailure("should have been instantiated away")
    }
  }

  internal func clone(into buffer: inout UnsafeMutableRawBufferPointer,
             endOfReferencePrefix: Bool) {
    var newHeader = header
    newHeader.endOfReferencePrefix = endOfReferencePrefix

    var componentSize = MemoryLayout<Header>.size
    buffer.storeBytes(of: newHeader, as: Header.self)
    switch header.kind {
    case .struct,
         .class:
      if header.storedOffsetPayload == Header.outOfLineOffsetPayload {
        let overflowOffset = body.load(as: UInt32.self)
        buffer.storeBytes(of: overflowOffset, toByteOffset: 4,
                          as: UInt32.self)
        componentSize += 4
      }
    case .optionalChain,
         .optionalForce,
         .optionalWrap:
      break
    case .computed:
      // Fields are pointer-aligned after the header
      componentSize += Header.pointerAlignmentSkew
      buffer.storeBytes(of: _computedIDValue,
                        toByteOffset: componentSize,
                        as: Int.self)
      componentSize += MemoryLayout<Int>.size
      let accessors = _computedAccessors

      (buffer.baseAddress.unsafelyUnwrapped + MemoryLayout<Int>.size * 2)
        ._copyAddressDiscriminatedFunctionPointer(
          from: accessors.getterPtr,
          discriminator: ComputedAccessorsPtr.getterPtrAuthKey)

      componentSize += MemoryLayout<Int>.size

      if header.isComputedSettable {
        (buffer.baseAddress.unsafelyUnwrapped + MemoryLayout<Int>.size * 3)
          ._copyAddressDiscriminatedFunctionPointer(
            from: accessors.setterPtr,
            discriminator: header.isComputedMutating
              ? ComputedAccessorsPtr.mutatingSetterPtrAuthKey
              : ComputedAccessorsPtr.nonmutatingSetterPtrAuthKey)
        componentSize += MemoryLayout<Int>.size
      }

      if header.hasComputedArguments {
        let arguments = _computedArguments
        let argumentSize = _computedArgumentSize
        buffer.storeBytes(of: argumentSize,
                          toByteOffset: componentSize,
                          as: Int.self)
        componentSize += MemoryLayout<Int>.size
        buffer.storeBytes(of: _computedArgumentWitnesses,
                          toByteOffset: componentSize,
                          as: ComputedArgumentWitnessesPtr.self)
        componentSize += MemoryLayout<Int>.size

        if header.isComputedInstantiatedFromExternalWithArguments {
          // Include the extra matter for components instantiated from
          // external property descriptors with arguments.
          buffer.storeBytes(of: _computedArgumentWitnessSizeAdjustment,
                            toByteOffset: componentSize,
                            as: Int.self)
          componentSize += MemoryLayout<Int>.size
        }
        let adjustedSize = argumentSize - _computedArgumentWitnessSizeAdjustment
        let argumentDest =
          buffer.baseAddress.unsafelyUnwrapped + componentSize
        _computedArgumentWitnesses.copy(
          arguments,
          argumentDest,
          adjustedSize)
        if header.isComputedInstantiatedFromExternalWithArguments {
          // The extra information for external property descriptor arguments
          // can always be memcpy'd.
          _memcpy(dest: argumentDest + adjustedSize,
                  src: arguments + adjustedSize,
                  size: UInt(_computedArgumentWitnessSizeAdjustment))
        }

        componentSize += argumentSize
      }

    case .external:
      _internalInvariantFailure("should have been instantiated away")
    }
    buffer = UnsafeMutableRawBufferPointer(
      start: buffer.baseAddress.unsafelyUnwrapped + componentSize,
      count: buffer.count - componentSize)
  }

  internal enum ProjectionResult<NewValue, LeafValue> {
    /// Continue projecting the key path with the given new value.
    case `continue`(NewValue)
    /// Stop projecting the key path and use the given value as the final
    /// result of the projection.
    case `break`(LeafValue)

    internal var assumingContinue: NewValue {
      switch self {
      case .continue(let x):
        return x
      case .break:
        _internalInvariantFailure("should not have stopped key path projection")
      }
    }
  }

  internal func _projectReadOnly<CurValue, NewValue, LeafValue>(
    _ base: CurValue,
    to: NewValue.Type,
    endingWith: LeafValue.Type
  ) -> ProjectionResult<NewValue, LeafValue> {
    switch value {
    case .struct(let offset):
      var base2 = base
      return .continue(withUnsafeBytes(of: &base2) {
        let p = $0.baseAddress.unsafelyUnwrapped.advanced(by: offset)
        // The contents of the struct should be well-typed, so we can assume
        // typed memory here.
        return p.assumingMemoryBound(to: NewValue.self).pointee
      })

    case .class(let offset):
      _internalInvariant(CurValue.self is AnyObject.Type,
                   "base is not a class")
      let baseObj = unsafeBitCast(base, to: AnyObject.self)
      let basePtr = UnsafeRawPointer(Builtin.bridgeToRawPointer(baseObj))
      defer { _fixLifetime(baseObj) }

      let offsetAddress = basePtr.advanced(by: offset)

      // Perform an instantaneous record access on the address in order to
      // ensure that the read will not conflict with an already in-progress
      // 'modify' access.
      Builtin.performInstantaneousReadAccess(offsetAddress._rawValue,
        NewValue.self)
      return .continue(offsetAddress
        .assumingMemoryBound(to: NewValue.self)
        .pointee)

    case .get(id: _, accessors: let accessors, argument: let argument),
         .mutatingGetSet(id: _, accessors: let accessors, argument: let argument),
         .nonmutatingGetSet(id: _, accessors: let accessors, argument: let argument):
      return .continue(accessors.getter()(base,
                               argument?.data.baseAddress ?? accessors._value,
                               argument?.data.count ?? 0))

    case .optionalChain:
      _internalInvariant(CurValue.self == Optional<NewValue>.self,
                   "should be unwrapping optional value")
      _internalInvariant(_isOptional(LeafValue.self),
                   "leaf result should be optional")
      if let baseValue = unsafeBitCast(base, to: Optional<NewValue>.self) {
        return .continue(baseValue)
      } else {
        // TODO: A more efficient way of getting the `none` representation
        // of a dynamically-optional type...
        return .break((Optional<()>.none as Any) as! LeafValue)
      }

    case .optionalForce:
      _internalInvariant(CurValue.self == Optional<NewValue>.self,
                   "should be unwrapping optional value")
      return .continue(unsafeBitCast(base, to: Optional<NewValue>.self)!)

    case .optionalWrap:
      _internalInvariant(NewValue.self == Optional<CurValue>.self,
                   "should be wrapping optional value")
      return .continue(
        unsafeBitCast(base as Optional<CurValue>, to: NewValue.self))
    }
  }

  internal func _projectMutableAddress<CurValue, NewValue>(
    _ base: UnsafeRawPointer,
    from _: CurValue.Type,
    to _: NewValue.Type,
    isRoot: Bool,
    keepAlive: inout AnyObject?
  ) -> UnsafeRawPointer {
    switch value {
    case .struct(let offset):
      return base.advanced(by: offset)
    case .class(let offset):
      // A class dereference should only occur at the root of a mutation,
      // since otherwise it would be part of the reference prefix.
      _internalInvariant(isRoot,
                 "class component should not appear in the middle of mutation")
      // AnyObject memory can alias any class reference memory, so we can
      // assume type here
      let object = base.assumingMemoryBound(to: AnyObject.self).pointee
      let offsetAddress = UnsafeRawPointer(Builtin.bridgeToRawPointer(object))
            .advanced(by: offset)

      // Keep the  base alive for the duration of the derived access and also
      // enforce exclusive access to the address.
      keepAlive = ClassHolder._create(previous: keepAlive, instance: object,
                                      accessingAddress: offsetAddress,
                                      type: NewValue.self)

      return offsetAddress
    
    case .mutatingGetSet(id: _, accessors: let accessors,
                         argument: let argument):
      let baseTyped = UnsafeMutablePointer(
        mutating: base.assumingMemoryBound(to: CurValue.self))

      let argValue = argument?.data.baseAddress ?? accessors._value
      let argSize = argument?.data.count ?? 0
      let writeback = MutatingWritebackBuffer<CurValue, NewValue>(
               previous: keepAlive,
               base: baseTyped,
               set: accessors.mutatingSetter(),
               argument: argValue,
               argumentSize: argSize,
               value: accessors.getter()(baseTyped.pointee, argValue, argSize))
      keepAlive = writeback
      // A maximally-abstracted, final, stored class property should have
      // a stable address.
      return UnsafeRawPointer(Builtin.addressof(&writeback.value))

    case .nonmutatingGetSet(id: _, accessors: let accessors,
                            argument: let argument):
      // A nonmutating property should only occur at the root of a mutation,
      // since otherwise it would be part of the reference prefix.
      _internalInvariant(isRoot,
           "nonmutating component should not appear in the middle of mutation")

      let baseValue = base.assumingMemoryBound(to: CurValue.self).pointee
      let argValue = argument?.data.baseAddress ?? accessors._value
      let argSize = argument?.data.count ?? 0
      let writeback = NonmutatingWritebackBuffer<CurValue, NewValue>(
                       previous: keepAlive,
                       base: baseValue,
                       set: accessors.nonmutatingSetter(),
                       argument: argValue,
                       argumentSize: argSize,
                       value: accessors.getter()(baseValue, argValue, argSize))
      keepAlive = writeback
      // A maximally-abstracted, final, stored class property should have
      // a stable address.
      return UnsafeRawPointer(Builtin.addressof(&writeback.value))

    case .optionalForce:
      _internalInvariant(CurValue.self == Optional<NewValue>.self,
                   "should be unwrapping an optional value")
      // Optional's layout happens to always put the payload at the start
      // address of the Optional value itself, if a value is present at all.
      let baseOptionalPointer
        = base.assumingMemoryBound(to: Optional<NewValue>.self)
      // Assert that a value exists
      _ = baseOptionalPointer.pointee!
      return base
    
    case .optionalChain, .optionalWrap, .get:
      _internalInvariantFailure("not a mutable key path component")
    }
  }
}

internal func _pop<T>(from: inout UnsafeRawBufferPointer,
                      as type: T.Type) -> T {
  let buffer = _pop(from: &from, as: type, count: 1)
  return buffer.baseAddress.unsafelyUnwrapped.pointee
}
internal func _pop<T>(from: inout UnsafeRawBufferPointer,
                      as: T.Type,
                      count: Int) -> UnsafeBufferPointer<T> {
  _internalInvariant(_isPOD(T.self), "should be POD")
  from = MemoryLayout<T>._roundingUpBaseToAlignment(from)
  let byteCount = MemoryLayout<T>.stride * count
  let result = UnsafeBufferPointer(
    start: from.baseAddress.unsafelyUnwrapped.assumingMemoryBound(to: T.self),
    count: count)

  from = UnsafeRawBufferPointer(
    start: from.baseAddress.unsafelyUnwrapped + byteCount,
    count: from.count - byteCount)
  return result
}
  
internal struct KeyPathBuffer {
  internal var data: UnsafeRawBufferPointer
  internal var trivial: Bool
  internal var hasReferencePrefix: Bool

  internal init(base: UnsafeRawPointer) {
    let header = base.load(as: Header.self)
    data = UnsafeRawBufferPointer(
      start: base + MemoryLayout<Int>.size,
      count: header.size)
    trivial = header.trivial
    hasReferencePrefix = header.hasReferencePrefix
  }

  internal init(partialData: UnsafeRawBufferPointer,
                trivial: Bool = false,
                hasReferencePrefix: Bool = false) {
    self.data = partialData
    self.trivial = trivial
    self.hasReferencePrefix = hasReferencePrefix
  }

  internal var mutableData: UnsafeMutableRawBufferPointer {
    return UnsafeMutableRawBufferPointer(mutating: data)
  }

  internal struct Builder {
    internal var buffer: UnsafeMutableRawBufferPointer
    internal init(_ buffer: UnsafeMutableRawBufferPointer) {
      self.buffer = buffer
    }
    internal mutating func pushRaw(size: Int, alignment: Int)
        -> UnsafeMutableRawBufferPointer {
      var baseAddress = buffer.baseAddress.unsafelyUnwrapped
      var misalign = Int(bitPattern: baseAddress) & (alignment - 1)
      if misalign != 0 {
        misalign = alignment - misalign
        baseAddress = baseAddress.advanced(by: misalign)
      }
      let result = UnsafeMutableRawBufferPointer(
        start: baseAddress,
        count: size)
      buffer = UnsafeMutableRawBufferPointer(
        start: baseAddress + size,
        count: buffer.count - size - misalign)
      return result
    }
    internal mutating func push<T>(_ value: T) {
      let buf = pushRaw(size: MemoryLayout<T>.size,
                        alignment: MemoryLayout<T>.alignment)
      buf.storeBytes(of: value, as: T.self)
    }
    internal mutating func pushHeader(_ header: Header) {
      push(header)
      // Start the components at pointer alignment
      _ = pushRaw(size: RawKeyPathComponent.Header.pointerAlignmentSkew,
             alignment: 4)
    }
  }

  internal struct Header {
    internal var _value: UInt32

    internal init(size: Int, trivial: Bool, hasReferencePrefix: Bool) {
      _internalInvariant(size <= Int(Header.sizeMask), "key path too big")
      _value = UInt32(size)
        | (trivial ? Header.trivialFlag : 0)
        | (hasReferencePrefix ? Header.hasReferencePrefixFlag : 0)
    }

    internal static var sizeMask: UInt32 {
      return _SwiftKeyPathBufferHeader_SizeMask
    }
    internal static var reservedMask: UInt32 {
      return _SwiftKeyPathBufferHeader_ReservedMask
    }
    internal static var trivialFlag: UInt32 {
      return _SwiftKeyPathBufferHeader_TrivialFlag
    }
    internal static var hasReferencePrefixFlag: UInt32 {
      return _SwiftKeyPathBufferHeader_HasReferencePrefixFlag
    }

    internal var size: Int { return Int(_value & Header.sizeMask) }
    internal var trivial: Bool { return _value & Header.trivialFlag != 0 }
    internal var hasReferencePrefix: Bool {
      get {
        return _value & Header.hasReferencePrefixFlag != 0
      }
      set {
        if newValue {
          _value |= Header.hasReferencePrefixFlag
        } else {
          _value &= ~Header.hasReferencePrefixFlag
        }
      }
    }

    // In a key path pattern, the "trivial" flag is used to indicate
    // "instantiable in-line"
    internal var instantiableInLine: Bool {
      return trivial
    }

    internal func validateReservedBits() {
      _precondition(_value & Header.reservedMask == 0,
                    "Reserved bits set to an unexpected bit pattern")
    }
  }

  internal func destroy() {
    // Short-circuit if nothing in the object requires destruction.
    if trivial { return }
    
    var bufferToDestroy = self
    while true {
      let (component, type) = bufferToDestroy.next()
      component.destroy()
      guard let _ = type else { break }
    }
  }
  
  internal mutating func next() -> (RawKeyPathComponent, Any.Type?) {
    let header = _pop(from: &data, as: RawKeyPathComponent.Header.self)
    // Track if this is the last component of the reference prefix.
    if header.endOfReferencePrefix {
      _internalInvariant(self.hasReferencePrefix,
                   "beginMutation marker in non-reference-writable key path?")
      self.hasReferencePrefix = false
    }
    
    var component = RawKeyPathComponent(header: header, body: data)
    // Shrinkwrap the component buffer size.
    let size = component.bodySize
    component.body = UnsafeRawBufferPointer(start: component.body.baseAddress,
                                            count: size)
    _ = _pop(from: &data, as: Int8.self, count: size)

    // fetch type, which is in the buffer unless it's the final component
    let nextType: Any.Type?
    if data.isEmpty {
      nextType = nil
    } else {
      nextType = _pop(from: &data, as: Any.Type.self)
    }
    return (component, nextType)
  }
}

// MARK: Library intrinsics for projecting key paths.

@_silgen_name("swift_getAtPartialKeyPath")
public // COMPILER_INTRINSIC
func _getAtPartialKeyPath<Root>(
  root: Root,
  keyPath: PartialKeyPath<Root>
) -> Any {
  func open<Value>(_: Value.Type) -> Any {
    return _getAtKeyPath(root: root,
      keyPath: unsafeDowncast(keyPath, to: KeyPath<Root, Value>.self))
  }
  return _openExistential(type(of: keyPath).valueType, do: open)
}

@_silgen_name("swift_getAtAnyKeyPath")
public // COMPILER_INTRINSIC
func _getAtAnyKeyPath<RootValue>(
  root: RootValue,
  keyPath: AnyKeyPath
) -> Any? {
  let (keyPathRoot, keyPathValue) = type(of: keyPath)._rootAndValueType
  func openRoot<KeyPathRoot>(_: KeyPathRoot.Type) -> Any? {
    guard let rootForKeyPath = root as? KeyPathRoot else {
      return nil
    }
    func openValue<Value>(_: Value.Type) -> Any {
      return _getAtKeyPath(root: rootForKeyPath,
        keyPath: unsafeDowncast(keyPath, to: KeyPath<KeyPathRoot, Value>.self))
    }
    return _openExistential(keyPathValue, do: openValue)
  }
  return _openExistential(keyPathRoot, do: openRoot)
}

@_silgen_name("swift_getAtKeyPath")
public // COMPILER_INTRINSIC
func _getAtKeyPath<Root, Value>(
  root: Root,
  keyPath: KeyPath<Root, Value>
) -> Value {
  return keyPath._projectReadOnly(from: root)
}

// The release that ends the access scope is guaranteed to happen
// immediately at the end_apply call because the continuation is a
// runtime call with a manual release (access scopes cannot be extended).
@_silgen_name("_swift_modifyAtWritableKeyPath_impl")
public // runtime entrypoint
func _modifyAtWritableKeyPath_impl<Root, Value>(
  root: inout Root,
  keyPath: WritableKeyPath<Root, Value>
) -> (UnsafeMutablePointer<Value>, AnyObject?) {
  if type(of: keyPath).kind == .reference {
    return _modifyAtReferenceWritableKeyPath_impl(root: root,
      keyPath: _unsafeUncheckedDowncast(keyPath,
        to: ReferenceWritableKeyPath<Root, Value>.self))
  }
  return _withUnprotectedUnsafePointer(to: &root) {
    keyPath._projectMutableAddress(from: $0)
  }
}

// The release that ends the access scope is guaranteed to happen
// immediately at the end_apply call because the continuation is a
// runtime call with a manual release (access scopes cannot be extended).
@_silgen_name("_swift_modifyAtReferenceWritableKeyPath_impl")
public // runtime entrypoint
func _modifyAtReferenceWritableKeyPath_impl<Root, Value>(
  root: Root,
  keyPath: ReferenceWritableKeyPath<Root, Value>
) -> (UnsafeMutablePointer<Value>, AnyObject?) {
  return keyPath._projectMutableAddress(from: root)
}

@_silgen_name("swift_setAtWritableKeyPath")
public // COMPILER_INTRINSIC
func _setAtWritableKeyPath<Root, Value>(
  root: inout Root,
  keyPath: WritableKeyPath<Root, Value>,
  value: __owned Value
) {
  if type(of: keyPath).kind == .reference {
    return _setAtReferenceWritableKeyPath(root: root,
      keyPath: _unsafeUncheckedDowncast(keyPath,
        to: ReferenceWritableKeyPath<Root, Value>.self),
      value: value)
  }
  // TODO: we should be able to do this more efficiently than projecting.
  let (addr, owner) = _withUnprotectedUnsafePointer(to: &root) {
    keyPath._projectMutableAddress(from: $0)
  }
  addr.pointee = value
  _fixLifetime(owner)
  // FIXME: this needs a deallocation barrier to ensure that the
  // release isn't extended, along with the access scope.
}

@_silgen_name("swift_setAtReferenceWritableKeyPath")
public // COMPILER_INTRINSIC
func _setAtReferenceWritableKeyPath<Root, Value>(
  root: Root,
  keyPath: ReferenceWritableKeyPath<Root, Value>,
  value: __owned Value
) {
  // TODO: we should be able to do this more efficiently than projecting.
  let (addr, owner) = keyPath._projectMutableAddress(from: root)
  addr.pointee = value
  _fixLifetime(owner)
  // FIXME: this needs a deallocation barrier to ensure that the
  // release isn't extended, along with the access scope.
}

// MARK: Appending type system

// FIXME(ABI): The type relationships between KeyPath append operands are tricky
// and don't interact well with our overriding rules. Hack things by injecting
// a bunch of `appending` overloads as protocol extensions so they aren't
// constrained by being overrides, and so that we can use exact-type constraints
// on `Self` to prevent dynamically-typed methods from being inherited by
// statically-typed key paths.

/// An implementation detail of key path expressions; do not use this protocol
/// directly.
@_show_in_interface
public protocol _AppendKeyPath {}

extension _AppendKeyPath where Self == AnyKeyPath {
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Appending the key path passed as `path` is successful only if the
  /// root type for `path` matches this key path's value type. This example
  /// creates key paths from `Array<Int>` to `String` and from `String` to
  /// `Int`, and then tries appending each to the other:
  ///
  ///     let arrayDescription: AnyKeyPath = \Array<Int>.description
  ///     let stringLength: AnyKeyPath = \String.count
  ///
  ///     // Creates a key path from `Array<Int>` to `Int`
  ///     let arrayDescriptionLength = arrayDescription.appending(path: stringLength)
  ///
  ///     let invalidKeyPath = stringLength.appending(path: arrayDescription)
  ///     // invalidKeyPath == nil
  ///
  /// The second call to `appending(path:)` returns `nil`
  /// because the root type of `arrayDescription`, `Array<Int>`, does not
  /// match the value type of `stringLength`, `Int`.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path and the value type
  ///   of `path`, if `path` can be appended. If `path` can't be appended,
  ///   returns `nil`.
  @inlinable
  public func appending(path: AnyKeyPath) -> AnyKeyPath? {
    return _tryToAppendKeyPaths(root: self, leaf: path)
  }
}

extension _AppendKeyPath /* where Self == PartialKeyPath<T> */ {
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Appending the key path passed as `path` is successful only if the
  /// root type for `path` matches this key path's value type. This example
  /// creates key paths from `Array<Int>` to `String` and from `String` to
  /// `Int`, and then tries appending each to the other:
  ///
  ///     let arrayDescription: PartialKeyPath<Array<Int>> = \.description
  ///     let stringLength: PartialKeyPath<String> = \.count
  ///
  ///     // Creates a key path from `Array<Int>` to `Int`
  ///     let arrayDescriptionLength = arrayDescription.appending(path: stringLength)
  ///
  ///     let invalidKeyPath = stringLength.appending(path: arrayDescription)
  ///     // invalidKeyPath == nil
  ///
  /// The second call to `appending(path:)` returns `nil`
  /// because the root type of `arrayDescription`, `Array<Int>`, does not
  /// match the value type of `stringLength`, `Int`.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path and the value type
  ///   of `path`, if `path` can be appended. If `path` can't be appended,
  ///   returns `nil`.
  @inlinable
  public func appending<Root>(path: AnyKeyPath) -> PartialKeyPath<Root>?
  where Self == PartialKeyPath<Root> {
    return _tryToAppendKeyPaths(root: self, leaf: path)
  }
  
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Appending the key path passed as `path` is successful only if the
  /// root type for `path` matches this key path's value type. This example
  /// creates a key path from `Array<Int>` to `String`, and then tries
  /// appending compatible and incompatible key paths:
  ///
  ///     let arrayDescription: PartialKeyPath<Array<Int>> = \.description
  ///
  ///     // Creates a key path from `Array<Int>` to `Int`
  ///     let arrayDescriptionLength = arrayDescription.appending(path: \String.count)
  ///
  ///     let invalidKeyPath = arrayDescription.appending(path: \Double.isZero)
  ///     // invalidKeyPath == nil
  ///
  /// The second call to `appending(path:)` returns `nil` because the root type
  /// of the `path` parameter, `Double`, does not match the value type of
  /// `arrayDescription`, `String`.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type
  ///   of `path`, if `path` can be appended. If `path` can't be appended,
  ///   returns `nil`.
  @inlinable
  public func appending<Root, AppendedRoot, AppendedValue>(
    path: KeyPath<AppendedRoot, AppendedValue>
  ) -> KeyPath<Root, AppendedValue>?
  where Self == PartialKeyPath<Root> {
    return _tryToAppendKeyPaths(root: self, leaf: path)
  }
  
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Appending the key path passed as `path` is successful only if the
  /// root type for `path` matches this key path's value type.
  ///
  /// - Parameter path: The reference writeable key path to append.
  /// - Returns: A key path from the root of this key path to the value type
  ///   of `path`, if `path` can be appended. If `path` can't be appended,
  ///   returns `nil`.
  @inlinable
  public func appending<Root, AppendedRoot, AppendedValue>(
    path: ReferenceWritableKeyPath<AppendedRoot, AppendedValue>
  ) -> ReferenceWritableKeyPath<Root, AppendedValue>?
  where Self == PartialKeyPath<Root> {
    return _tryToAppendKeyPaths(root: self, leaf: path)
  }
}

extension _AppendKeyPath /* where Self == KeyPath<T,U> */ {
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Calling `appending(path:)` results in the same key path as if the
  /// given key path had been specified using dot notation. In the following
  /// example, `keyPath1` and `keyPath2` are equivalent:
  ///
  ///     let arrayDescription = \Array<Int>.description
  ///     let keyPath1 = arrayDescription.appending(path: \String.count)
  ///
  ///     let keyPath2 = \Array<Int>.description.count
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type of
  ///   `path`.
  @inlinable
  public func appending<Root, Value, AppendedValue>(
    path: KeyPath<Value, AppendedValue>
  ) -> KeyPath<Root, AppendedValue>
  where Self: KeyPath<Root, Value> {
    return _appendingKeyPaths(root: self, leaf: path)
  }

  /* TODO
  public func appending<Root, Value, Leaf>(
    path: Leaf,
    // FIXME: Satisfy "Value generic param not used in signature" constraint
    _: Value.Type = Value.self
  ) -> PartialKeyPath<Root>?
  where Self: KeyPath<Root, Value>, Leaf == AnyKeyPath {
    return _tryToAppendKeyPaths(root: self, leaf: path)
  }
   */

  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Calling `appending(path:)` results in the same key path as if the
  /// given key path had been specified using dot notation.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type of
  ///   `path`.
  @inlinable
  public func appending<Root, Value, AppendedValue>(
    path: ReferenceWritableKeyPath<Value, AppendedValue>
  ) -> ReferenceWritableKeyPath<Root, AppendedValue>
  where Self == KeyPath<Root, Value> {
    return _appendingKeyPaths(root: self, leaf: path)
  }
}

extension _AppendKeyPath /* where Self == WritableKeyPath<T,U> */ {
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Calling `appending(path:)` results in the same key path as if the
  /// given key path had been specified using dot notation.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type of
  ///   `path`.
  @inlinable
  public func appending<Root, Value, AppendedValue>(
    path: WritableKeyPath<Value, AppendedValue>
  ) -> WritableKeyPath<Root, AppendedValue>
  where Self == WritableKeyPath<Root, Value> {
    return _appendingKeyPaths(root: self, leaf: path)
  }

  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Calling `appending(path:)` results in the same key path as if the
  /// given key path had been specified using dot notation.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type of
  ///   `path`.
  @inlinable
  public func appending<Root, Value, AppendedValue>(
    path: ReferenceWritableKeyPath<Value, AppendedValue>
  ) -> ReferenceWritableKeyPath<Root, AppendedValue>
  where Self == WritableKeyPath<Root, Value> {
    return _appendingKeyPaths(root: self, leaf: path)
  }
}

extension _AppendKeyPath /* where Self == ReferenceWritableKeyPath<T,U> */ {
  /// Returns a new key path created by appending the given key path to this
  /// one.
  ///
  /// Use this method to extend this key path to the value type of another key
  /// path. Calling `appending(path:)` results in the same key path as if the
  /// given key path had been specified using dot notation.
  ///
  /// - Parameter path: The key path to append.
  /// - Returns: A key path from the root of this key path to the value type of
  ///   `path`.
  @inlinable
  public func appending<Root, Value, AppendedValue>(
    path: WritableKeyPath<Value, AppendedValue>
  ) -> ReferenceWritableKeyPath<Root, AppendedValue>
  where Self == ReferenceWritableKeyPath<Root, Value> {
    return _appendingKeyPaths(root: self, leaf: path)
  }
}

/// Updates information pertaining to the types associated with each KeyPath.
///
/// Note: Currently we only distinguish between keypaths that traverse
/// only structs to get to the final value, and all other types.
/// This is done for performance reasons.
/// Other type information may be handled in the future to improve performance.
internal func _processOffsetForAppendedKeyPath(
  appendedKeyPath: inout AnyKeyPath,
  root: AnyKeyPath,
  leaf: AnyKeyPath
) {
  if let rootOffset = root.getOffsetFromStorage(),
    let leafOffset = leaf.getOffsetFromStorage()
  {
    appendedKeyPath.assignOffsetToStorage(offset: rootOffset + leafOffset)
  }
}

@usableFromInline
internal func _tryToAppendKeyPaths<Result: AnyKeyPath>(
  root: AnyKeyPath,
  leaf: AnyKeyPath
) -> Result? {
  let (rootRoot, rootValue) = type(of: root)._rootAndValueType
  let (leafRoot, leafValue) = type(of: leaf)._rootAndValueType
  
  if rootValue != leafRoot {
    return nil
  }
  
  func open<Root>(_: Root.Type) -> Result {
    func open2<Value>(_: Value.Type) -> Result {
      func open3<AppendedValue>(_: AppendedValue.Type) -> Result {
        let typedRoot = unsafeDowncast(root, to: KeyPath<Root, Value>.self)
        let typedLeaf = unsafeDowncast(leaf,
                                       to: KeyPath<Value, AppendedValue>.self)
        var result:AnyKeyPath = _appendingKeyPaths(root: typedRoot,
                                                   leaf: typedLeaf)
        _processOffsetForAppendedKeyPath(appendedKeyPath: &result,
          root: root, leaf: leaf)
        return unsafeDowncast(result, to: Result.self)
      }
      return _openExistential(leafValue, do: open3)
    }
    return _openExistential(rootValue, do: open2)
  }
  return _openExistential(rootRoot, do: open)
}

@usableFromInline
internal func _appendingKeyPaths<
  Root, Value, AppendedValue,
  Result: KeyPath<Root, AppendedValue>
>(
  root: KeyPath<Root, Value>,
  leaf: KeyPath<Value, AppendedValue>
) -> Result {
  let resultTy = type(of: root).appendedType(with: type(of: leaf))
    var returnValue: AnyKeyPath = root.withBuffer {
    var rootBuffer = $0
    return leaf.withBuffer {
      var leafBuffer = $0

      // If either operand is the identity key path, then we should return
      // the other operand back untouched.
      if leafBuffer.data.isEmpty {
        return unsafeDowncast(root, to: Result.self)
      }
      if rootBuffer.data.isEmpty {
        return unsafeDowncast(leaf, to: Result.self)
      }

      // Reserve room for the appended KVC string, if both key paths are
      // KVC-compatible.
      let appendedKVCLength: Int, rootKVCLength: Int, leafKVCLength: Int

      if root.getOffsetFromStorage() == nil, leaf.getOffsetFromStorage() == nil,
        let rootPtr = root._kvcKeyPathStringPtr,
        let leafPtr = leaf._kvcKeyPathStringPtr {
        rootKVCLength = Int(_swift_stdlib_strlen(rootPtr))
        leafKVCLength = Int(_swift_stdlib_strlen(leafPtr))
        // root + "." + leaf
        appendedKVCLength = rootKVCLength + 1 + leafKVCLength + 1
      } else {
        rootKVCLength = 0
        leafKVCLength = 0
        appendedKVCLength = 0
      }

      // Result buffer has room for both key paths' components, plus the
      // header, plus space for the middle type.
      // Align up the root so that we can put the component type after it.
      let rootSize = MemoryLayout<Int>._roundingUpToAlignment(rootBuffer.data.count)
      let resultSize = rootSize + leafBuffer.data.count
        + 2 * MemoryLayout<Int>.size
      // Tail-allocate space for the KVC string.
      let totalResultSize = MemoryLayout<Int32>
        ._roundingUpToAlignment(resultSize + appendedKVCLength)

      var kvcStringBuffer: UnsafeMutableRawPointer? = nil

      let result = resultTy._create(capacityInBytes: totalResultSize) {
        var destBuffer = $0

        // Remember where the tail-allocated KVC string buffer begins.
        if appendedKVCLength > 0 {
          kvcStringBuffer = destBuffer.baseAddress.unsafelyUnwrapped
            .advanced(by: resultSize)

          destBuffer = .init(start: destBuffer.baseAddress,
                             count: resultSize)
        }
        
        var destBuilder = KeyPathBuffer.Builder(destBuffer)
        
        // Save space for the header.
        let leafIsReferenceWritable = type(of: leaf).kind == .reference
        destBuilder.pushHeader(KeyPathBuffer.Header(
          size: resultSize - MemoryLayout<Int>.size,
          trivial: rootBuffer.trivial && leafBuffer.trivial,
          hasReferencePrefix: rootBuffer.hasReferencePrefix
                              || leafIsReferenceWritable
        ))
        
        let leafHasReferencePrefix = leafBuffer.hasReferencePrefix
        
        // Clone the root components into the buffer.
        
        while true {
          let (component, type) = rootBuffer.next()
          let isLast = type == nil
          // If the leaf appended path has a reference prefix, then the
          // entire root is part of the reference prefix.
          let endOfReferencePrefix: Bool
          if leafHasReferencePrefix {
            endOfReferencePrefix = false
          } else if isLast && leafIsReferenceWritable {
            endOfReferencePrefix = true
          } else {
            endOfReferencePrefix = component.header.endOfReferencePrefix
          }
          
          component.clone(
            into: &destBuilder.buffer,
            endOfReferencePrefix: endOfReferencePrefix)
          // Insert our endpoint type between the root and leaf components.
          if let type = type {
            destBuilder.push(type)
          } else {
            destBuilder.push(Value.self as Any.Type)
            break
          }
        }
        
        // Clone the leaf components into the buffer.
        while true {
          let (component, type) = leafBuffer.next()

          component.clone(
            into: &destBuilder.buffer,
            endOfReferencePrefix: component.header.endOfReferencePrefix)

          if let type = type {
            destBuilder.push(type)
          } else {
            break
          }
        }
        
        _internalInvariant(destBuilder.buffer.isEmpty,
                     "did not fill entire result buffer")
      }

      // Build the KVC string if there is one.
      if root.getOffsetFromStorage() == nil,
        leaf.getOffsetFromStorage() == nil {
        if let kvcStringBuffer = kvcStringBuffer {
          let rootPtr = root._kvcKeyPathStringPtr.unsafelyUnwrapped
          let leafPtr = leaf._kvcKeyPathStringPtr.unsafelyUnwrapped
          _memcpy(
            dest: kvcStringBuffer,
            src: rootPtr,
            size: UInt(rootKVCLength))
          kvcStringBuffer.advanced(by: rootKVCLength)
            .storeBytes(of: 0x2E /* '.' */, as: CChar.self)
          _memcpy(
            dest: kvcStringBuffer.advanced(by: rootKVCLength + 1),
            src: leafPtr,
            size: UInt(leafKVCLength))
          result._kvcKeyPathStringPtr =
            UnsafePointer(kvcStringBuffer.assumingMemoryBound(to: CChar.self))
          kvcStringBuffer.advanced(by: rootKVCLength + leafKVCLength + 1)
            .storeBytes(of: 0 /* '\0' */, as: CChar.self)
        }
      }
      return unsafeDowncast(result, to: Result.self)
    }
  }
  _processOffsetForAppendedKeyPath(
    appendedKeyPath: &returnValue,
    root: root,
    leaf: leaf
  )
  return returnValue as! Result
}

// The distance in bytes from the address point of a KeyPath object to its
// buffer header. Includes the size of the Swift heap object header and the
// pointer to the KVC string.

internal var keyPathObjectHeaderSize: Int {
  return MemoryLayout<HeapObject>.size + MemoryLayout<Int>.size
}

internal var keyPathPatternHeaderSize: Int {
  return 16
}

// Runtime entry point to instantiate a key path object.
// Note that this has a compatibility override shim in the runtime so that
// future compilers can backward-deploy support for instantiating new key path
// pattern features.
@_cdecl("swift_getKeyPathImpl")
public func _swift_getKeyPath(pattern: UnsafeMutableRawPointer,
                              arguments: UnsafeRawPointer)
    -> UnsafeRawPointer {
  // The key path pattern is laid out like a key path object, with a few
  // modifications:
  // - Pointers in the instantiated object are compressed into 32-bit
  //   relative offsets in the pattern.
  // - The pattern begins with a field that's either zero, for a pattern that
  //   depends on instantiation arguments, or that's a relative reference to
  //   a global mutable pointer variable, which can be initialized to a single
  //   shared instantiation of this pattern.
  // - Instead of the two-word object header with isa and refcount, two
  //   pointers to metadata accessors are provided for the root and leaf
  //   value types of the key path.
  // - Components may have unresolved forms that require instantiation.
  // - Type metadata and protocol conformance pointers are replaced with
  //   relative-referenced accessor functions that instantiate the
  //   needed generic argument when called.
  //
  // The pattern never precomputes the capabilities of the key path (readonly/
  // writable/reference-writable), nor does it encode the reference prefix.
  // These are resolved dynamically, so that they always reflect the dynamic
  // capability of the properties involved.

  let oncePtrPtr = pattern
  let patternPtr = pattern.advanced(by: 4)

  let bufferHeader = patternPtr.load(fromByteOffset: keyPathPatternHeaderSize,
                                     as: KeyPathBuffer.Header.self)
  bufferHeader.validateReservedBits()

  // If the first word is nonzero, it relative-references a cache variable
  // we can use to reference a single shared instantiation of this key path.
  let oncePtrOffset = oncePtrPtr.load(as: Int32.self)
  let oncePtr: UnsafeRawPointer?
  if oncePtrOffset != 0 {
    let theOncePtr = _resolveRelativeAddress(oncePtrPtr, oncePtrOffset)
    oncePtr = theOncePtr

    // See whether we already instantiated this key path.
    // This is a non-atomic load because the instantiated pointer will be
    // written with a release barrier, and loads of the instantiated key path
    // ought to carry a dependency through this loaded pointer.
    let existingInstance = theOncePtr.load(as: UnsafeRawPointer?.self)
    
    if let existingInstance = existingInstance {
      // Return the instantiated object at +1.
      let object = Unmanaged<AnyKeyPath>.fromOpaque(existingInstance)
      // TODO: This retain will be unnecessary once we support global objects
      // with inert refcounting.
      _ = object.retain()
      return existingInstance
    }
  } else {
    oncePtr = nil
  }

  // Instantiate a new key path object modeled on the pattern.
  // Do a pass to determine the class of the key path we'll be instantiating
  // and how much space we'll need for it.
  let (keyPathClass, rootType, size, _)
    = _getKeyPathClassAndInstanceSizeFromPattern(patternPtr, arguments)

  var pureStructOffset: UInt32? = nil
        
  // Allocate the instance.
  let instance = keyPathClass._create(capacityInBytes: size) { instanceData in
    // Instantiate the pattern into the instance.
    pureStructOffset = _instantiateKeyPathBuffer(
      patternPtr,
      instanceData,
      rootType,
      arguments
    )
  }

  // Adopt the KVC string from the pattern.
  let kvcStringBase = patternPtr.advanced(by: 12)
  let kvcStringOffset = kvcStringBase.load(as: Int32.self)

  if kvcStringOffset == 0 {
    // Null pointer.
    instance._kvcKeyPathStringPtr = nil
  } else {
    let kvcStringPtr = _resolveRelativeAddress(kvcStringBase, kvcStringOffset)
    instance._kvcKeyPathStringPtr =
      kvcStringPtr.assumingMemoryBound(to: CChar.self)
  }
  if instance._kvcKeyPathStringPtr == nil, let offset = pureStructOffset {
    instance.assignOffsetToStorage(offset: Int(offset))
  }
  // If we can cache this instance as a shared instance, do so.
  if let oncePtr = oncePtr {
    // Try to replace a null pointer in the cache variable with the instance
    // pointer.
    let instancePtr = Unmanaged.passRetained(instance)

    while true {
      let (oldValue, won) = Builtin.cmpxchg_seqcst_seqcst_Word(
        oncePtr._rawValue,
        0._builtinWordValue,
        UInt(bitPattern: instancePtr.toOpaque())._builtinWordValue)

      // If the exchange succeeds, then the instance we formed is the canonical
      // one.
      if Bool(won) {
        break
      }

      // Otherwise, someone raced with us to instantiate the key path pattern
      // and won. Their instance should be just as good as ours, so we can take
      // that one and let ours get deallocated.
      if let existingInstance = UnsafeRawPointer(bitPattern: Int(oldValue)) {
        // Return the instantiated object at +1.
        let object = Unmanaged<AnyKeyPath>.fromOpaque(existingInstance)
        // TODO: This retain will be unnecessary once we support global objects
        // with inert refcounting.
        _ = object.retain()
        // Release the instance we created.
        instancePtr.release()
        return existingInstance
      } else {
        // Try the cmpxchg again if it spuriously failed.
        continue
      }
    }
  }

  return UnsafeRawPointer(Unmanaged.passRetained(instance).toOpaque())
}

// A reference to metadata, which is a pointer to a mangled name.
internal typealias MetadataReference = UnsafeRawPointer

// Determine the length of the given mangled name.
internal func _getSymbolicMangledNameLength(_ base: UnsafeRawPointer) -> Int {
  var end = base
  while let current = Optional(end.load(as: UInt8.self)), current != 0 {
    // Skip the current character
    end = end + 1

    // Skip over a symbolic reference
    if current >= 0x1 && current <= 0x17 {
      end += 4
    } else if current >= 0x18 && current <= 0x1F {
      end += MemoryLayout<Int>.size
    }
  }

  return end - base
}

// Resolve a mangled name in a generic environment, described by either a
// flat GenericEnvironment * (if the bottom tag bit is 0) or possibly-nested
// ContextDescriptor * (if the bottom tag bit is 1)
internal func _getTypeByMangledNameInEnvironmentOrContext(
  _ name: UnsafePointer<UInt8>,
  _ nameLength: UInt,
  genericEnvironmentOrContext: UnsafeRawPointer?,
  genericArguments: UnsafeRawPointer?)
  -> Any.Type? {

  let taggedPointer = UInt(bitPattern: genericEnvironmentOrContext)
  if taggedPointer & 1 == 0 {
    return _getTypeByMangledNameInEnvironment(name, nameLength,
                      genericEnvironment: genericEnvironmentOrContext,
                      genericArguments: genericArguments)
  } else {
    let context = UnsafeRawPointer(bitPattern: taggedPointer & ~1)
    return _getTypeByMangledNameInContext(name, nameLength,
                      genericContext: context,
                      genericArguments: genericArguments)
  }
}

// Resolve the given generic argument reference to a generic argument.
internal func _resolveKeyPathGenericArgReference(
    _ reference: UnsafeRawPointer,
    genericEnvironment: UnsafeRawPointer?,
    arguments: UnsafeRawPointer?)
    -> UnsafeRawPointer {
  // If the low bit is clear, it's a direct reference to the argument.
  if (UInt(bitPattern: reference) & 0x01 == 0) {
    return reference
  }

  // Adjust the reference.
  let referenceStart = reference - 1

  // If we have a symbolic reference to an accessor, call it.
  let first = referenceStart.load(as: UInt8.self)
  if first == 255 && reference.load(as: UInt8.self) == 9 {
    typealias MetadataAccessor =
      @convention(c) (UnsafeRawPointer?) -> UnsafeRawPointer

    // Unaligned load of the offset.
    let pointerReference = reference + 1
    var offset: Int32 = 0
    _memcpy(dest: &offset, src: pointerReference, size: 4)

    let accessorPtrRaw = _resolveCompactFunctionPointer(pointerReference, offset)
    let accessorPtrSigned =
      _PtrAuth.sign(pointer: accessorPtrRaw,
              key: .processIndependentCode,
              discriminator: _PtrAuth.discriminator(for: MetadataAccessor.self))
    let accessor = unsafeBitCast(accessorPtrSigned, to: MetadataAccessor.self)
    return accessor(arguments)
  }

  let nameLength = _getSymbolicMangledNameLength(referenceStart)
  let namePtr = referenceStart.bindMemory(to: UInt8.self,
                                          capacity: nameLength + 1)
  // FIXME: Could extract this information from the mangled name.
  guard let result =
    _getTypeByMangledNameInEnvironmentOrContext(namePtr, UInt(nameLength),
                         genericEnvironmentOrContext: genericEnvironment,
                         genericArguments: arguments)
  else {
    let nameStr = String._fromUTF8Repairing(
      UnsafeBufferPointer(start: namePtr, count: nameLength)
    ).0

    fatalError("could not demangle keypath type from '\(nameStr)'")
  }

  return unsafeBitCast(result, to: UnsafeRawPointer.self)
}

// Resolve the given metadata reference to (type) metadata.
internal func _resolveKeyPathMetadataReference(
    _ reference: UnsafeRawPointer,
    genericEnvironment: UnsafeRawPointer?,
    arguments: UnsafeRawPointer?)
    -> Any.Type {
  return unsafeBitCast(
           _resolveKeyPathGenericArgReference(
             reference,
             genericEnvironment: genericEnvironment,
             arguments: arguments),
           to: Any.Type.self)
}

internal enum KeyPathStructOrClass {
  case `struct`, `class`
}
internal enum KeyPathPatternStoredOffset {
  case inline(UInt32)
  case outOfLine(UInt32)
  case unresolvedFieldOffset(UInt32)
  case unresolvedIndirectOffset(UnsafePointer<UInt>)
}
internal struct KeyPathPatternComputedArguments {
  var getLayout: KeyPathComputedArgumentLayoutFn
  var witnesses: ComputedArgumentWitnessesPtr
  var initializer: KeyPathComputedArgumentInitializerFn
}

internal protocol KeyPathPatternVisitor {
  mutating func visitHeader(genericEnvironment: UnsafeRawPointer?,
                            rootMetadataRef: MetadataReference,
                            leafMetadataRef: MetadataReference,
                            kvcCompatibilityString: UnsafeRawPointer?)
  mutating func visitStoredComponent(kind: KeyPathStructOrClass,
                                     mutable: Bool,
                                     offset: KeyPathPatternStoredOffset)
  mutating func visitComputedComponent(mutating: Bool,
                                       idKind: KeyPathComputedIDKind,
                                       idResolution: KeyPathComputedIDResolution,
                                       idValueBase: UnsafeRawPointer,
                                       idValue: Int32,
                                       getter: UnsafeRawPointer,
                                       setter: UnsafeRawPointer?,
                                       arguments: KeyPathPatternComputedArguments?,
                                       externalArgs: UnsafeBufferPointer<Int32>?)
  mutating func visitOptionalChainComponent()
  mutating func visitOptionalForceComponent()
  mutating func visitOptionalWrapComponent()

  mutating func visitIntermediateComponentType(metadataRef: MetadataReference)

  mutating func finish()
}

internal func _resolveRelativeAddress(_ base: UnsafeRawPointer,
                                      _ offset: Int32) -> UnsafeRawPointer {
  // Sign-extend the offset to pointer width and add with wrap on overflow.
  return UnsafeRawPointer(bitPattern: Int(bitPattern: base) &+ Int(offset))
    .unsafelyUnwrapped
}
internal func _resolveRelativeIndirectableAddress(_ base: UnsafeRawPointer,
                                                  _ offset: Int32)
    -> UnsafeRawPointer {
  // Low bit indicates whether the reference is indirected or not.
  if offset & 1 != 0 {
    let ptrToPtr = _resolveRelativeAddress(base, offset - 1)
    return ptrToPtr.load(as: UnsafeRawPointer.self)
  }
  return _resolveRelativeAddress(base, offset)
}

internal func _resolveCompactFunctionPointer(_ base: UnsafeRawPointer, _ offset: Int32)
    -> UnsafeRawPointer {
#if SWIFT_COMPACT_ABSOLUTE_FUNCTION_POINTER
  return UnsafeRawPointer(bitPattern: Int(offset)).unsafelyUnwrapped
#else
  return _resolveRelativeAddress(base, offset)
#endif
}

internal func _loadRelativeAddress<T>(at: UnsafeRawPointer,
                                      fromByteOffset: Int = 0,
                                      as: T.Type) -> T {
  let offset = at.load(fromByteOffset: fromByteOffset, as: Int32.self)
  return unsafeBitCast(_resolveRelativeAddress(at + fromByteOffset, offset),
                       to: T.self)
}

internal func _walkKeyPathPattern<W: KeyPathPatternVisitor>(
                                  _ pattern: UnsafeRawPointer,
                                  walker: inout W) {
  // Visit the header.
  let genericEnvironment = _loadRelativeAddress(at: pattern,
                                                as: UnsafeRawPointer.self)
  let rootMetadataRef = _loadRelativeAddress(at: pattern, fromByteOffset: 4,
                                             as: MetadataReference.self)
  let leafMetadataRef = _loadRelativeAddress(at: pattern, fromByteOffset: 8,
                                             as: MetadataReference.self)
  let kvcString = _loadRelativeAddress(at: pattern, fromByteOffset: 12,
                                       as: UnsafeRawPointer.self)

  walker.visitHeader(genericEnvironment: genericEnvironment,
                     rootMetadataRef: rootMetadataRef,
                     leafMetadataRef: leafMetadataRef,
                     kvcCompatibilityString: kvcString)

  func visitStored(header: RawKeyPathComponent.Header,
                   componentBuffer: inout UnsafeRawBufferPointer) {
    // Decode a stored property. A small offset may be stored inline in the
    // header word, or else be stored out-of-line, or need instantiation of some
    // kind.
    let offset: KeyPathPatternStoredOffset
    switch header.storedOffsetPayload {
    case RawKeyPathComponent.Header.outOfLineOffsetPayload:
      offset = .outOfLine(_pop(from: &componentBuffer,
                               as: UInt32.self))
    case RawKeyPathComponent.Header.unresolvedFieldOffsetPayload:
      offset = .unresolvedFieldOffset(_pop(from: &componentBuffer,
                                           as: UInt32.self))
    case RawKeyPathComponent.Header.unresolvedIndirectOffsetPayload:
      let base = componentBuffer.baseAddress.unsafelyUnwrapped
      let relativeOffset = _pop(from: &componentBuffer,
                                as: Int32.self)
      let ptr = _resolveRelativeIndirectableAddress(base, relativeOffset)
      offset = .unresolvedIndirectOffset(
                                       ptr.assumingMemoryBound(to: UInt.self))
    default:
      offset = .inline(header.storedOffsetPayload)
    }
    let kind: KeyPathStructOrClass = header.kind == .struct 
      ? .struct : .class
    walker.visitStoredComponent(kind: kind,
                                mutable: header.isStoredMutable,
                                offset: offset)
  }

  func popComputedAccessors(header: RawKeyPathComponent.Header,
                            componentBuffer: inout UnsafeRawBufferPointer)
      -> (idValueBase: UnsafeRawPointer,
          idValue: Int32,
          getter: UnsafeRawPointer,
          setter: UnsafeRawPointer?) {
    let idValueBase = componentBuffer.baseAddress.unsafelyUnwrapped
    let idValue = _pop(from: &componentBuffer, as: Int32.self)
    let getterBase = componentBuffer.baseAddress.unsafelyUnwrapped
    let getterRef = _pop(from: &componentBuffer, as: Int32.self)
    let getter = _resolveCompactFunctionPointer(getterBase, getterRef)
    let setter: UnsafeRawPointer?
    if header.isComputedSettable {
      let setterBase = componentBuffer.baseAddress.unsafelyUnwrapped
      let setterRef = _pop(from: &componentBuffer, as: Int32.self)
      setter = _resolveCompactFunctionPointer(setterBase, setterRef)
    } else {
      setter = nil
    }
    return (idValueBase: idValueBase, idValue: idValue,
            getter: getter, setter: setter)
  }

  func popComputedArguments(header: RawKeyPathComponent.Header,
                            componentBuffer: inout UnsafeRawBufferPointer)
      -> KeyPathPatternComputedArguments? {
    if header.hasComputedArguments {
      let getLayoutBase = componentBuffer.baseAddress.unsafelyUnwrapped
      let getLayoutRef = _pop(from: &componentBuffer, as: Int32.self)
      let getLayoutRaw = _resolveCompactFunctionPointer(getLayoutBase, getLayoutRef)
      let getLayoutSigned = _PtrAuth.sign(pointer: getLayoutRaw,
        key: .processIndependentCode,
        discriminator: _PtrAuth.discriminator(for: KeyPathComputedArgumentLayoutFn.self))
      let getLayout = unsafeBitCast(getLayoutSigned,
                                    to: KeyPathComputedArgumentLayoutFn.self)

      let witnessesBase = componentBuffer.baseAddress.unsafelyUnwrapped
      let witnessesRef = _pop(from: &componentBuffer, as: Int32.self)
      let witnesses: UnsafeRawPointer
      if witnessesRef == 0 {
        witnesses = __swift_keyPathGenericWitnessTable_addr()
      } else {
        witnesses = _resolveRelativeAddress(witnessesBase, witnessesRef)
      }

      let initializerBase = componentBuffer.baseAddress.unsafelyUnwrapped
      let initializerRef = _pop(from: &componentBuffer, as: Int32.self)
      let initializerRaw = _resolveCompactFunctionPointer(initializerBase,
                                                          initializerRef)
      let initializerSigned = _PtrAuth.sign(pointer: initializerRaw,
        key: .processIndependentCode,
        discriminator: _PtrAuth.discriminator(for: KeyPathComputedArgumentInitializerFn.self))

      let initializer = unsafeBitCast(initializerSigned,
                                  to: KeyPathComputedArgumentInitializerFn.self)

      return KeyPathPatternComputedArguments(getLayout: getLayout,
        witnesses: ComputedArgumentWitnessesPtr(witnesses),
        initializer: initializer)
    } else {
      return nil
    }
  }

  // We declare this down here to avoid the temptation to use it within
  // the functions above.
  let bufferPtr = pattern.advanced(by: keyPathPatternHeaderSize)
  let bufferHeader = bufferPtr.load(as: KeyPathBuffer.Header.self)
  var buffer = UnsafeRawBufferPointer(start: bufferPtr + 4,
                                      count: bufferHeader.size)

  while !buffer.isEmpty {
    let header = _pop(from: &buffer,
                      as: RawKeyPathComponent.Header.self)

    // Ensure that we pop an amount of data consistent with what
    // RawKeyPathComponent.Header.patternComponentBodySize computes.
    var bufferSizeBefore = 0
    var expectedPop = 0

    _internalInvariant({
      bufferSizeBefore = buffer.count
      expectedPop = header.patternComponentBodySize
      return true
    }())

    switch header.kind {
    case .class, .struct:
      visitStored(header: header, componentBuffer: &buffer)
    case .computed:
      let (idValueBase, idValue, getter, setter)
        = popComputedAccessors(header: header,
                               componentBuffer: &buffer)

      // If there are arguments, gather those too.
      let arguments = popComputedArguments(header: header,
                                           componentBuffer: &buffer)

      walker.visitComputedComponent(mutating: header.isComputedMutating,
                                    idKind: header.computedIDKind,
                                    idResolution: header.computedIDResolution,
                                    idValueBase: idValueBase,
                                    idValue: idValue,
                                    getter: getter,
                                    setter: setter,
                                    arguments: arguments,
                                    externalArgs: nil)

    case .optionalChain:
      walker.visitOptionalChainComponent()
    case .optionalWrap:
      walker.visitOptionalWrapComponent()
    case .optionalForce:
      walker.visitOptionalForceComponent()
    case .external:
      // Look at the external property descriptor to see if we should take it
      // over the component given in the pattern.
      let genericParamCount = Int(header.payload)
      let descriptorBase = buffer.baseAddress.unsafelyUnwrapped
      let descriptorOffset = _pop(from: &buffer,
                                  as: Int32.self)
      let descriptor =
        _resolveRelativeIndirectableAddress(descriptorBase, descriptorOffset)
      let descriptorHeader =
        descriptor.load(as: RawKeyPathComponent.Header.self)
      if descriptorHeader.isTrivialPropertyDescriptor {
        // If the descriptor is trivial, then use the local candidate.
        // Skip the external generic parameter accessors to get to it.
        _ = _pop(from: &buffer, as: Int32.self, count: genericParamCount)
        continue
      }
      
      // Grab the generic parameter accessors to pass to the external component.
      let externalArgs = _pop(from: &buffer, as: Int32.self,
                              count: genericParamCount)

      // Grab the header for the local candidate in case we need it for
      // a computed property.
      let localCandidateHeader = _pop(from: &buffer,
                                      as: RawKeyPathComponent.Header.self)
      let localCandidateSize = localCandidateHeader.patternComponentBodySize
      _internalInvariant({
        expectedPop += localCandidateSize + 4
        return true
      }())

      let descriptorSize = descriptorHeader.propertyDescriptorBodySize
      var descriptorBuffer = UnsafeRawBufferPointer(start: descriptor + 4,
                                                    count: descriptorSize)

      // Look at what kind of component the external property has.
      switch descriptorHeader.kind {
      case .struct, .class:
        // A stored component. We can instantiate it
        // without help from the local candidate.
        _ = _pop(from: &buffer, as: UInt8.self, count: localCandidateSize)

        visitStored(header: descriptorHeader,
                    componentBuffer: &descriptorBuffer)
        
      case .computed:
        // A computed component. The accessors come from the descriptor.
        let (idValueBase, idValue, getter, setter)
          = popComputedAccessors(header: descriptorHeader,
                                 componentBuffer: &descriptorBuffer)
        
        // Get the arguments from the external descriptor and/or local candidate
        // component.
        let arguments: KeyPathPatternComputedArguments?
        if localCandidateHeader.kind == .computed
            && localCandidateHeader.hasComputedArguments {
          // If both have arguments, then we have to build a bit of a chimera.
          // The canonical identity and accessors come from the descriptor,
          // but the argument equality/hash handling is still as described
          // in the local candidate.
          // We don't need the local candidate's accessors.
          _ = popComputedAccessors(header: localCandidateHeader,
                                   componentBuffer: &buffer)
          // We do need the local arguments.
          arguments = popComputedArguments(header: localCandidateHeader,
                                           componentBuffer: &buffer)
        } else {
          // If the local candidate doesn't have arguments, we don't need
          // anything from it at all.
          _ = _pop(from: &buffer, as: UInt8.self, count: localCandidateSize)
          arguments = nil
        }

        walker.visitComputedComponent(
          mutating: descriptorHeader.isComputedMutating,
          idKind: descriptorHeader.computedIDKind,
          idResolution: descriptorHeader.computedIDResolution,
          idValueBase: idValueBase,
          idValue: idValue,
          getter: getter,
          setter: setter,
          arguments: arguments,
          externalArgs: genericParamCount > 0 ? externalArgs : nil)
      case .optionalChain, .optionalWrap, .optionalForce, .external:
        _internalInvariantFailure("not possible for property descriptor")
      }
    }

    // Check that we consumed the expected amount of data from the pattern.
    _internalInvariant(
      {
        // Round the amount of data we read up to alignment.
        let popped = MemoryLayout<Int32>._roundingUpToAlignment(
           bufferSizeBefore - buffer.count)
        return expectedPop == popped
      }(),
      """
      component size consumed during pattern walk does not match \
      component size returned by patternComponentBodySize
      """)

    // Break if this is the last component.
    if buffer.isEmpty { break }

    // Otherwise, pop the intermediate component type accessor and
    // go around again.
    let componentTypeBase = buffer.baseAddress.unsafelyUnwrapped
    let componentTypeOffset = _pop(from: &buffer, as: Int32.self)
    let componentTypeRef = _resolveRelativeAddress(componentTypeBase,
                                                   componentTypeOffset)
    walker.visitIntermediateComponentType(metadataRef: componentTypeRef)
    _internalInvariant(!buffer.isEmpty)
  }

  // We should have walked the entire pattern.
  _internalInvariant(buffer.isEmpty, "did not walk entire pattern buffer")
  walker.finish()
}

internal struct GetKeyPathClassAndInstanceSizeFromPattern
    : KeyPathPatternVisitor {
  var size: Int = MemoryLayout<Int>.size // start with one word for the header
  var capability: KeyPathKind = .value
  var didChain: Bool = false
  var root: Any.Type!
  var leaf: Any.Type!
  var genericEnvironment: UnsafeRawPointer?
  let patternArgs: UnsafeRawPointer?
  var structOffset: UInt32 = 0
  var isPureStruct: [Bool] = []

  init(patternArgs: UnsafeRawPointer?) {
    self.patternArgs = patternArgs
  }

  mutating func roundUpToPointerAlignment() {
    size = MemoryLayout<Int>._roundingUpToAlignment(size)
  }

  mutating func visitHeader(genericEnvironment: UnsafeRawPointer?,
                            rootMetadataRef: MetadataReference,
                            leafMetadataRef: MetadataReference,
                            kvcCompatibilityString: UnsafeRawPointer?) {
    self.genericEnvironment = genericEnvironment
    // Get the root and leaf type metadata so we can form the class type
    // for the entire key path.
    root = _resolveKeyPathMetadataReference(
              rootMetadataRef,
              genericEnvironment: genericEnvironment,
              arguments: patternArgs)
    leaf = _resolveKeyPathMetadataReference(
              leafMetadataRef,
              genericEnvironment: genericEnvironment,
              arguments: patternArgs)
  }

  mutating func visitStoredComponent(kind: KeyPathStructOrClass,
                                     mutable: Bool,
                                     offset: KeyPathPatternStoredOffset) {
    // Mutable class properties can be the root of a reference mutation.
    // Mutable struct properties pass through the existing capability.
    if mutable {
      switch kind {
      case .class:
        capability = .reference
      case .struct:
        break
      }
    } else {
      // Immutable properties can only be read.
      capability = .readOnly
    }

    // The size of the instantiated component depends on whether we can fit
    // the offset inline.
    switch offset {
    case .inline:
      size += 4

    case .outOfLine, .unresolvedFieldOffset, .unresolvedIndirectOffset:
      size += 8
    }
  }

  mutating func visitComputedComponent(mutating: Bool,
                                   idKind: KeyPathComputedIDKind,
                                   idResolution: KeyPathComputedIDResolution,
                                   idValueBase: UnsafeRawPointer,
                                   idValue: Int32,
                                   getter: UnsafeRawPointer,
                                   setter: UnsafeRawPointer?,
                                   arguments: KeyPathPatternComputedArguments?,
                                   externalArgs: UnsafeBufferPointer<Int32>?) {
    let settable = setter != nil

    switch (settable, mutating) {
    case (false, false):
      // If the property is get-only, the capability becomes read-only, unless
      // we get another reference-writable component.
      capability = .readOnly
    case (true, false):
      capability = .reference
    case (true, true):
      // Writable if the base is. No effect.
      break
    case (false, true):
      _internalInvariantFailure("unpossible")
    }

    // Save space for the header...
    size += 4
    roundUpToPointerAlignment()
    // ...id, getter, and maybe setter...
    size += MemoryLayout<Int>.size * 2
    if settable {
      size += MemoryLayout<Int>.size
    }
    
    // ...and the arguments, if any.
    let argumentHeaderSize = MemoryLayout<Int>.size * 2
    switch (arguments, externalArgs) {
    case (nil, nil):
      break
    case (let arguments?, nil):
      size += argumentHeaderSize
      // If we have arguments, calculate how much space they need by invoking
      // the layout function.
      let (addedSize, addedAlignmentMask) = arguments.getLayout(patternArgs)
      // TODO: Handle over-aligned values
      _internalInvariant(addedAlignmentMask < MemoryLayout<Int>.alignment,
                   "overaligned computed property element not supported")
      size += addedSize
    
    case (let arguments?, let externalArgs?):
      // If we're referencing an external declaration, and it takes captured
      // arguments, then we have to build a bit of a chimera. The canonical
      // identity and accessors come from the descriptor, but the argument
      // handling is still as described in the local candidate.
      size += argumentHeaderSize
      let (addedSize, addedAlignmentMask) = arguments.getLayout(patternArgs)
      // TODO: Handle over-aligned values
      _internalInvariant(addedAlignmentMask < MemoryLayout<Int>.alignment,
                   "overaligned computed property element not supported")
      size += addedSize
      // We also need to store the size of the local arguments so we can
      // find the external component arguments.
      roundUpToPointerAlignment()
      size += RawKeyPathComponent.Header.externalWithArgumentsExtraSize
      size += MemoryLayout<Int>.size * externalArgs.count

    case (nil, let externalArgs?):
      // If we're instantiating an external property with a local
      // candidate that has no arguments, then things are a little
      // easier. We only need to instantiate the generic
      // arguments for the external component's accessors.
      size += argumentHeaderSize
      size += MemoryLayout<Int>.size * externalArgs.count
    }
  }

  mutating func visitOptionalChainComponent() {
    // Optional chaining forces the entire keypath to be read-only, even if
    // there are further reference-writable components.
    didChain = true
    capability = .readOnly
    size += 4
  }
  mutating func visitOptionalWrapComponent() {
    // Optional chaining forces the entire keypath to be read-only, even if
    // there are further reference-writable components.
    didChain = true
    capability = .readOnly
    size += 4
  }

  mutating func visitOptionalForceComponent() {
    // Force-unwrapping passes through the mutability of the preceding keypath.
    size += 4
  }

  mutating
  func visitIntermediateComponentType(metadataRef _: MetadataReference) {
    // The instantiated component type will be stored in the instantiated
    // object.
    roundUpToPointerAlignment()
    size += MemoryLayout<Int>.size
  }

  mutating func finish() {
  }
}

internal func _getKeyPathClassAndInstanceSizeFromPattern(
  _ pattern: UnsafeRawPointer,
  _ arguments: UnsafeRawPointer
) -> (
  keyPathClass: AnyKeyPath.Type,
  rootType: Any.Type,
  size: Int,
  alignmentMask: Int
) {
  var walker = GetKeyPathClassAndInstanceSizeFromPattern(patternArgs: arguments)
  _walkKeyPathPattern(pattern, walker: &walker)

  // Chaining always renders the whole key path read-only.
  if walker.didChain {
    walker.capability = .readOnly
  }

  // Grab the class object for the key path type we'll end up with.
  func openRoot<Root>(_: Root.Type) -> AnyKeyPath.Type {
    func openLeaf<Leaf>(_: Leaf.Type) -> AnyKeyPath.Type {
      switch walker.capability {
      case .readOnly:
        return KeyPath<Root, Leaf>.self
      case .value:
        return WritableKeyPath<Root, Leaf>.self
      case .reference:
        return ReferenceWritableKeyPath<Root, Leaf>.self
      }
    }
    return _openExistential(walker.leaf!, do: openLeaf)
  }
  let classTy = _openExistential(walker.root!, do: openRoot)

  return (keyPathClass: classTy,
          rootType: walker.root!,
          size: walker.size,
          // FIXME: Handle overalignment
          alignmentMask: MemoryLayout<Int>._alignmentMask)
}

internal struct InstantiateKeyPathBuffer: KeyPathPatternVisitor {
  var destData: UnsafeMutableRawBufferPointer
  var genericEnvironment: UnsafeRawPointer?
  let patternArgs: UnsafeRawPointer?
  var base: Any.Type
  var structOffset: UInt32 = 0
  var isPureStruct: [Bool] = []

  init(destData: UnsafeMutableRawBufferPointer,
       patternArgs: UnsafeRawPointer?,
       root: Any.Type) {
    self.destData = destData
    self.patternArgs = patternArgs
    self.base = root
  }

  // Track the triviality of the resulting object data.
  var isTrivial: Bool = true

  // Track where the reference prefix begins.
  var endOfReferencePrefixComponent: UnsafeMutableRawPointer? = nil
  var previousComponentAddr: UnsafeMutableRawPointer? = nil

  mutating func adjustDestForAlignment<T>(of: T.Type) -> (
    baseAddress: UnsafeMutableRawPointer,
    misalign: Int
  ) {
    let alignment = MemoryLayout<T>.alignment
    var baseAddress = destData.baseAddress.unsafelyUnwrapped
    var misalign = Int(bitPattern: baseAddress) & (alignment - 1)
    if misalign != 0 {
      misalign = alignment - misalign
      baseAddress = baseAddress.advanced(by: misalign)
    }
    return (baseAddress, misalign)
  }
  mutating func pushDest<T>(_ value: T) {
    _internalInvariant(_isPOD(T.self))
    let size = MemoryLayout<T>.size
    let (baseAddress, misalign) = adjustDestForAlignment(of: T.self)
    _withUnprotectedUnsafeBytes(of: value) {
      _memcpy(dest: baseAddress, src: $0.baseAddress.unsafelyUnwrapped,
              size: UInt(size))
    }
    destData = UnsafeMutableRawBufferPointer(
      start: baseAddress + size,
      count: destData.count - size - misalign)
  }
  mutating func pushAddressDiscriminatedFunctionPointer(
    _ unsignedPointer: UnsafeRawPointer,
    discriminator: UInt64
  ) {
    let size = MemoryLayout<UnsafeRawPointer>.size
    let (baseAddress, misalign) =
      adjustDestForAlignment(of: UnsafeRawPointer.self)
    baseAddress._storeFunctionPointerWithAddressDiscrimination(
      unsignedPointer, discriminator: discriminator)
    destData = UnsafeMutableRawBufferPointer(
      start: baseAddress + size,
      count: destData.count - size - misalign)
  }

  mutating func updatePreviousComponentAddr() -> UnsafeMutableRawPointer? {
    let oldValue = previousComponentAddr
    previousComponentAddr = destData.baseAddress.unsafelyUnwrapped
    return oldValue
  }

  mutating func visitHeader(genericEnvironment: UnsafeRawPointer?,
                            rootMetadataRef: MetadataReference,
                            leafMetadataRef: MetadataReference,
                            kvcCompatibilityString: UnsafeRawPointer?) {
    self.genericEnvironment = genericEnvironment
  }

  mutating func visitStoredComponent(kind: KeyPathStructOrClass,
                                     mutable: Bool,
                                     offset: KeyPathPatternStoredOffset) {
    let previous = updatePreviousComponentAddr()
    switch kind {
        case .struct:
      isPureStruct.append(true)
        default:
      isPureStruct.append(false)
    }
    switch kind {
    case .class:
      // A mutable class property can end the reference prefix.
      if mutable {
        endOfReferencePrefixComponent = previous
      }
      fallthrough

    case .struct:
      // Resolve the offset.
      switch offset {
      case .inline(let value):
        let header = RawKeyPathComponent.Header(stored: kind,
                                                mutable: mutable,
                                                inlineOffset: value)
        pushDest(header)
        switch kind {
          case .struct:
            structOffset += value
          default:
             break
        }
      case .outOfLine(let offset):
        let header = RawKeyPathComponent.Header(storedWithOutOfLineOffset: kind,
                                                mutable: mutable)
        pushDest(header)
        pushDest(offset)
      case .unresolvedFieldOffset(let offsetOfOffset):
        // Look up offset in the type metadata. The value in the pattern is
        // the offset within the metadata object.
        let metadataPtr = unsafeBitCast(base, to: UnsafeRawPointer.self)
        let offset: UInt32
        switch kind {
        case .class:
          offset = UInt32(metadataPtr.load(fromByteOffset: Int(offsetOfOffset),
                                           as: UInt.self))
        case .struct:
          offset = UInt32(metadataPtr.load(fromByteOffset: Int(offsetOfOffset),
                                           as: UInt32.self))
          structOffset += offset
        }

        let header = RawKeyPathComponent.Header(storedWithOutOfLineOffset: kind,
                                                mutable: mutable)
        pushDest(header)
        pushDest(offset)
      case .unresolvedIndirectOffset(let pointerToOffset):
        // Look up offset in the indirectly-referenced variable we have a
        // pointer.
        _internalInvariant(pointerToOffset.pointee <= UInt32.max)
        let offset = UInt32(truncatingIfNeeded: pointerToOffset.pointee)
        let header = RawKeyPathComponent.Header(storedWithOutOfLineOffset: kind,
                                                mutable: mutable)
        pushDest(header)
        pushDest(offset)
      }
    }
  }

  mutating func visitComputedComponent(mutating: Bool,
                                   idKind: KeyPathComputedIDKind,
                                   idResolution: KeyPathComputedIDResolution,
                                   idValueBase: UnsafeRawPointer,
                                   idValue: Int32,
                                   getter: UnsafeRawPointer,
                                   setter: UnsafeRawPointer?,
                                   arguments: KeyPathPatternComputedArguments?,
                                   externalArgs: UnsafeBufferPointer<Int32>?) {
    isPureStruct.append(false)
    let previous = updatePreviousComponentAddr()
    let settable = setter != nil
    // A nonmutating settable property can end the reference prefix.
    if settable && !mutating {
      endOfReferencePrefixComponent = previous
    }

    // Resolve the ID.
    let resolvedID: UnsafeRawPointer?

    switch idKind {
    case .storedPropertyIndex, .vtableOffset:
      _internalInvariant(idResolution == .resolved)
      // Zero-extend the integer value to get the instantiated id.
      let value = UInt(UInt32(bitPattern: idValue))
      resolvedID = UnsafeRawPointer(bitPattern: value)

    case .pointer:
      // If the pointer ID is unresolved, then it needs work to get to
      // the final value.
      switch idResolution {
      case .resolved:
        resolvedID = _resolveRelativeAddress(idValueBase, idValue)
        break

      case .resolvedAbsolute:
        let value = UInt(UInt32(bitPattern: idValue))
        resolvedID = UnsafeRawPointer(bitPattern: value)
        break

      case .indirectPointer:
        // The pointer in the pattern is an indirect pointer to the real
        // identifier pointer.
        let absoluteID = _resolveRelativeAddress(idValueBase, idValue)
        resolvedID = absoluteID
          .load(as: UnsafeRawPointer?.self)

      case .functionCall:
        // The pointer in the pattern is to a function that generates the
        // identifier pointer.
        typealias Resolver = @convention(c) (UnsafeRawPointer?) -> UnsafeRawPointer?
        let absoluteID = _resolveCompactFunctionPointer(idValueBase, idValue)
        let resolverSigned = _PtrAuth.sign(
          pointer: absoluteID,
          key: .processIndependentCode,
          discriminator: _PtrAuth.discriminator(for: Resolver.self))
        let resolverFn = unsafeBitCast(resolverSigned,
                                       to: Resolver.self)

        resolvedID = resolverFn(patternArgs)
      }
    }

    // Bring over the header, getter, and setter.
    let header = RawKeyPathComponent.Header(computedWithIDKind: idKind,
          mutating: mutating,
          settable: settable,
          hasArguments: arguments != nil || externalArgs != nil,
          instantiatedFromExternalWithArguments:
            arguments != nil && externalArgs != nil)
    pushDest(header)
    pushDest(resolvedID)
    pushAddressDiscriminatedFunctionPointer(getter,
                           discriminator: ComputedAccessorsPtr.getterPtrAuthKey)
    if let setter = setter {
      pushAddressDiscriminatedFunctionPointer(setter,
        discriminator: mutating ? ComputedAccessorsPtr.mutatingSetterPtrAuthKey
                             : ComputedAccessorsPtr.nonmutatingSetterPtrAuthKey)
    }

    if let arguments = arguments {
      // Instantiate the arguments.
      let (baseSize, alignmentMask) = arguments.getLayout(patternArgs)
      _internalInvariant(alignmentMask < MemoryLayout<Int>.alignment,
                   "overaligned computed arguments not implemented yet")

      // The real buffer stride will be rounded up to alignment.
      var totalSize = (baseSize + alignmentMask) & ~alignmentMask

      // If an external property descriptor also has arguments, they'll be
      // added to the end with pointer alignment.
      if let externalArgs = externalArgs {
        totalSize = MemoryLayout<Int>._roundingUpToAlignment(totalSize)
        totalSize += MemoryLayout<Int>.size * externalArgs.count
      }

      pushDest(totalSize)
      pushDest(arguments.witnesses)

      // A nonnull destructor in the witnesses file indicates the instantiated
      // payload is nontrivial.
      if let _ = arguments.witnesses.destroy {
        isTrivial = false
      }

      // If the descriptor has arguments, store the size of its specific
      // arguments here, so we can drop them when trying to invoke
      // the component's witnesses.
      if let externalArgs = externalArgs {
        pushDest(externalArgs.count * MemoryLayout<Int>.size)
      }

      // Initialize the local candidate arguments here.
      _internalInvariant(Int(bitPattern: destData.baseAddress) & alignmentMask == 0,
                   "argument destination not aligned")
      arguments.initializer(patternArgs,
                            destData.baseAddress.unsafelyUnwrapped)

      destData = UnsafeMutableRawBufferPointer(
        start: destData.baseAddress.unsafelyUnwrapped + baseSize,
        count: destData.count - baseSize)
    }
    
    if let externalArgs = externalArgs {
      if arguments == nil {
        // If we're instantiating an external property without any local
        // arguments, then we only need to instantiate the arguments to the
        // property descriptor.
        let stride = MemoryLayout<Int>.size * externalArgs.count
        pushDest(stride)
        pushDest(__swift_keyPathGenericWitnessTable_addr())
      }

      // Write the descriptor's generic arguments, which should all be relative
      // references to metadata accessor functions.
      for i in externalArgs.indices {
        let base = externalArgs.baseAddress.unsafelyUnwrapped + i
        let offset = base.pointee
        let metadataRef = _resolveRelativeAddress(UnsafeRawPointer(base), offset)
        let result = _resolveKeyPathGenericArgReference(
                       metadataRef,
                       genericEnvironment: genericEnvironment,
                       arguments: patternArgs)
        pushDest(result)
      }
    }
  }

  mutating func visitOptionalChainComponent() {
    isPureStruct.append(false)
    let _ = updatePreviousComponentAddr()
    let header = RawKeyPathComponent.Header(optionalChain: ())
    pushDest(header)
  }
  mutating func visitOptionalWrapComponent() {
    isPureStruct.append(false)
    let _ = updatePreviousComponentAddr()
    let header = RawKeyPathComponent.Header(optionalWrap: ())
    pushDest(header)
  }
  mutating func visitOptionalForceComponent() {
    isPureStruct.append(false)
    let _ = updatePreviousComponentAddr()
    let header = RawKeyPathComponent.Header(optionalForce: ())
    pushDest(header)
  }

  mutating func visitIntermediateComponentType(metadataRef: MetadataReference) {
    // Get the metadata for the intermediate type.
    let metadata = _resolveKeyPathMetadataReference(
                     metadataRef,
                     genericEnvironment: genericEnvironment,
                     arguments: patternArgs)
    pushDest(metadata)
    base = metadata
  }
  
  mutating func finish() {
    // Should have filled the entire buffer by the time we reach the end of the
    // pattern.
    _internalInvariant(destData.isEmpty,
                 "should have filled entire destination buffer")
  }
}

#if INTERNAL_CHECKS_ENABLED
// In debug builds of the standard library, check that instantiation produces
// components whose sizes are consistent with the sizing visitor pass.
internal struct ValidatingInstantiateKeyPathBuffer: KeyPathPatternVisitor {
  var sizeVisitor: GetKeyPathClassAndInstanceSizeFromPattern
  var instantiateVisitor: InstantiateKeyPathBuffer
  let origDest: UnsafeMutableRawPointer
  var structOffset: UInt32 = 0
  var isPureStruct: [Bool] = []

  init(sizeVisitor: GetKeyPathClassAndInstanceSizeFromPattern,
       instantiateVisitor: InstantiateKeyPathBuffer) {
    self.sizeVisitor = sizeVisitor
    self.instantiateVisitor = instantiateVisitor
    origDest = self.instantiateVisitor.destData.baseAddress.unsafelyUnwrapped
  }

  mutating func visitHeader(genericEnvironment: UnsafeRawPointer?,
                            rootMetadataRef: MetadataReference,
                            leafMetadataRef: MetadataReference,
                            kvcCompatibilityString: UnsafeRawPointer?) {
    sizeVisitor.visitHeader(genericEnvironment: genericEnvironment,
                            rootMetadataRef: rootMetadataRef,
                            leafMetadataRef: leafMetadataRef,
                            kvcCompatibilityString: kvcCompatibilityString)
    instantiateVisitor.visitHeader(genericEnvironment: genericEnvironment,
                                 rootMetadataRef: rootMetadataRef,
                                 leafMetadataRef: leafMetadataRef,
                                 kvcCompatibilityString: kvcCompatibilityString)
  }
  mutating func visitStoredComponent(kind: KeyPathStructOrClass,
                                     mutable: Bool,
                                     offset: KeyPathPatternStoredOffset) {
    sizeVisitor.visitStoredComponent(kind: kind, mutable: mutable,
                                     offset: offset)
    instantiateVisitor.visitStoredComponent(kind: kind, mutable: mutable,
                                            offset: offset)
    checkSizeConsistency()
    structOffset = instantiateVisitor.structOffset
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
  }
  mutating func visitComputedComponent(mutating: Bool,
                                   idKind: KeyPathComputedIDKind,
                                   idResolution: KeyPathComputedIDResolution,
                                   idValueBase: UnsafeRawPointer,
                                   idValue: Int32,
                                   getter: UnsafeRawPointer,
                                   setter: UnsafeRawPointer?,
                                   arguments: KeyPathPatternComputedArguments?,
                                   externalArgs: UnsafeBufferPointer<Int32>?) {
    sizeVisitor.visitComputedComponent(mutating: mutating,
                                       idKind: idKind,
                                       idResolution: idResolution,
                                       idValueBase: idValueBase,
                                       idValue: idValue,
                                       getter: getter,
                                       setter: setter,
                                       arguments: arguments,
                                       externalArgs: externalArgs)
    instantiateVisitor.visitComputedComponent(mutating: mutating,
                                       idKind: idKind,
                                       idResolution: idResolution,
                                       idValueBase: idValueBase,
                                       idValue: idValue,
                                       getter: getter,
                                       setter: setter,
                                       arguments: arguments,
                                       externalArgs: externalArgs)
    // Note: For this function and the ones below, modification of structOffset
    // is omitted since these types of KeyPaths won't have a pureStruct
    // offset anyway.
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }
  mutating func visitOptionalChainComponent() {
    sizeVisitor.visitOptionalChainComponent()
    instantiateVisitor.visitOptionalChainComponent()
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }
  mutating func visitOptionalWrapComponent() {
    sizeVisitor.visitOptionalWrapComponent()
    instantiateVisitor.visitOptionalWrapComponent()
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }
  mutating func visitOptionalForceComponent() {
    sizeVisitor.visitOptionalForceComponent()
    instantiateVisitor.visitOptionalForceComponent()
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }
  mutating func visitIntermediateComponentType(metadataRef: MetadataReference) {
    sizeVisitor.visitIntermediateComponentType(metadataRef: metadataRef)
    instantiateVisitor.visitIntermediateComponentType(metadataRef: metadataRef)
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }

  mutating func finish() {
    sizeVisitor.finish()
    instantiateVisitor.finish()
    isPureStruct.append(contentsOf: instantiateVisitor.isPureStruct)
    checkSizeConsistency()
  }

  func checkSizeConsistency() {
    let nextDest = instantiateVisitor.destData.baseAddress.unsafelyUnwrapped
    let curSize = nextDest - origDest + MemoryLayout<Int>.size

    _internalInvariant(curSize == sizeVisitor.size,
                 "size and instantiation visitors out of sync")
  }
}
#endif // INTERNAL_CHECKS_ENABLED

internal func _instantiateKeyPathBuffer(
  _ pattern: UnsafeRawPointer,
  _ origDestData: UnsafeMutableRawBufferPointer,
  _ rootType: Any.Type,
  _ arguments: UnsafeRawPointer
) -> UInt32? {
  let destHeaderPtr = origDestData.baseAddress.unsafelyUnwrapped
  var destData = UnsafeMutableRawBufferPointer(
    start: destHeaderPtr.advanced(by: MemoryLayout<Int>.size),
    count: origDestData.count - MemoryLayout<Int>.size)

#if INTERNAL_CHECKS_ENABLED
  // If checks are enabled, use a validating walker that ensures that the
  // size pre-walk and instantiation walk are in sync.
  let sizeWalker = GetKeyPathClassAndInstanceSizeFromPattern(
    patternArgs: arguments)
  let instantiateWalker = InstantiateKeyPathBuffer(
    destData: destData,
    patternArgs: arguments,
    root: rootType)
  
  var walker = ValidatingInstantiateKeyPathBuffer(sizeVisitor: sizeWalker,
                                          instantiateVisitor: instantiateWalker)
#else
  var walker = InstantiateKeyPathBuffer(
    destData: destData,
    patternArgs: arguments,
    root: rootType)
#endif

  _walkKeyPathPattern(pattern, walker: &walker)

#if INTERNAL_CHECKS_ENABLED
  let isTrivial = walker.instantiateVisitor.isTrivial
  let endOfReferencePrefixComponent =
    walker.instantiateVisitor.endOfReferencePrefixComponent
#else
  let isTrivial = walker.isTrivial
  let endOfReferencePrefixComponent = walker.endOfReferencePrefixComponent
#endif

  // Write out the header.
  let destHeader = KeyPathBuffer.Header(
    size: origDestData.count - MemoryLayout<Int>.size,
    trivial: isTrivial,
    hasReferencePrefix: endOfReferencePrefixComponent != nil)

  destHeaderPtr.storeBytes(of: destHeader, as: KeyPathBuffer.Header.self)

  // Mark the reference prefix if there is one.
  if let endOfReferencePrefixComponent = endOfReferencePrefixComponent {
    var componentHeader = endOfReferencePrefixComponent
      .load(as: RawKeyPathComponent.Header.self)
    componentHeader.endOfReferencePrefix = true
    endOfReferencePrefixComponent.storeBytes(of: componentHeader,
      as: RawKeyPathComponent.Header.self)
  }
  var isPureStruct = true
  var offset: UInt32? = nil
      
  for value in walker.isPureStruct {
    isPureStruct = isPureStruct && value
  }

  if isPureStruct {
      offset = walker.structOffset
  }
  return offset
}

#if SWIFT_ENABLE_REFLECTION

@available(SwiftStdlib 5.9, *)
public func _createOffsetBasedKeyPath(
  root: Any.Type,
  value: Any.Type,
  offset: Int
) -> AnyKeyPath {
  func openRoot<Root>(_: Root.Type) -> AnyKeyPath.Type {
    func openValue<Value>(_: Value.Type) -> AnyKeyPath.Type {
      KeyPath<Root, Value>.self
    }

    return _openExistential(value, do: openValue(_:))
  }

  let kpTy = _openExistential(root, do: openRoot(_:))

  // The buffer header is 32 bits, but components must start on a word
  // boundary.
  let kpBufferSize = MemoryLayout<Int>.size + MemoryLayout<Int32>.size
  let kp = kpTy._create(capacityInBytes: kpBufferSize) {
    var builder = KeyPathBuffer.Builder($0)
    let header = KeyPathBuffer.Header(
      size: kpBufferSize - MemoryLayout<Int>.size,
      trivial: true,
      hasReferencePrefix: false
    )

    builder.pushHeader(header)

    let componentHeader = RawKeyPathComponent.Header(
      stored: _MetadataKind(root) == .struct ? .struct : .class,
      mutable: false,
      inlineOffset: UInt32(offset)
    )

    let component = RawKeyPathComponent(
      header: componentHeader,
      body: UnsafeRawBufferPointer(start: nil, count: 0)
    )

    component.clone(into: &builder.buffer, endOfReferencePrefix: false)
  }

  if _MetadataKind(root) == .struct {
    kp.assignOffsetToStorage(offset: offset)
  }

  return kp
}

@_spi(ObservableRerootKeyPath)
@available(SwiftStdlib 5.9, *)
public func _rerootKeyPath<NewRoot>(
  _ existingKp: AnyKeyPath,
  to newRoot: NewRoot.Type
) -> PartialKeyPath<NewRoot> {
  let (isTrivial, hasReferencePrefix, componentSize) = existingKp.withBuffer {
    ($0.trivial, $0.hasReferencePrefix, $0.data.count)
  }

  let existingKpTy = type(of: existingKp)

  func openedRoot<Root>(_: Root.Type) -> AnyKeyPath.Type {
    func openedValue<Value>(_: Value.Type) -> AnyKeyPath.Type {
      if existingKpTy == ReferenceWritableKeyPath<Root, Value>.self {
        return ReferenceWritableKeyPath<NewRoot, Value>.self
      } else if existingKpTy == KeyPath<Root, Value>.self {
        return KeyPath<NewRoot, Value>.self
      } else {
        fatalError("Unsupported KeyPath type to be rerooted")
      }
    }

    return _openExistential(existingKpTy.valueType, do: openedValue(_:))
  }

  let newKpTy = _openExistential(existingKpTy.rootType, do: openedRoot(_:))

  return newKpTy._create(
    // This is the buffer header + padding (if needed) + size of components
    capacityInBytes: MemoryLayout<Int>.size + componentSize
  ) {
    var builder = KeyPathBuffer.Builder($0)
    let header = KeyPathBuffer.Header(
      size: componentSize,
      trivial: isTrivial,
      hasReferencePrefix: hasReferencePrefix
    )

    builder.pushHeader(header)

    existingKp.withBuffer {
      var existingBuffer = $0

      while true {
        let (rawComponent, componentTy) = existingBuffer.next()

        rawComponent.clone(
          into: &builder.buffer,
          endOfReferencePrefix: rawComponent.header.endOfReferencePrefix
        )

        if componentTy == nil {
          break
        }
      }
    }
  } as! PartialKeyPath<NewRoot>
}

@_silgen_name("swift_keyPath_copySymbolName")
fileprivate func keyPath_copySymbolName(
  _: UnsafeRawPointer
) -> UnsafePointer<CChar>?

@_silgen_name("swift_keyPath_freeSymbolName")
fileprivate func keyPath_freeSymbolName(
  _: UnsafePointer<CChar>?
) -> Void

@_silgen_name("swift_keyPathSourceString")
fileprivate func demangle(
  name: UnsafePointer<CChar>
) -> UnsafeMutablePointer<CChar>?

fileprivate func dynamicLibraryAddress<Base, Leaf>(
  of pointer: ComputedAccessorsPtr,
  _: Base.Type,
  _ leaf: Leaf.Type
) -> String {
  let getter: ComputedAccessorsPtr.Getter<Base, Leaf> = pointer.getter()
  let pointer = unsafeBitCast(getter, to: UnsafeRawPointer.self)
  if let cString = keyPath_copySymbolName(UnsafeRawPointer(pointer)) {
    defer {
      keyPath_freeSymbolName(cString)
    }
    if let demangled = demangle(name: cString)
      .map({ pointer in
        defer {
          pointer.deallocate()
        }
        return String(cString: pointer)
    }) {
      return demangled
    }
  }
  return "<computed \(pointer) (\(leaf))>"
}

#endif

@available(SwiftStdlib 5.8, *)
extension AnyKeyPath: CustomDebugStringConvertible {
  
#if SWIFT_ENABLE_REFLECTION
  @available(SwiftStdlib 5.8, *)
  public var debugDescription: String {
    var description = "\\\(String(describing: Self.rootType))"
    return withBuffer {
      var buffer = $0
      if buffer.data.isEmpty {
        description.append(".self")
        return description
      }
      var valueType: Any.Type = Self.rootType
      while true {
        let (rawComponent, optNextType) = buffer.next()
        let hasEnded = optNextType == nil
        let nextType = optNextType ?? Self.valueType
        switch rawComponent.value {
        case .optionalForce, .optionalWrap, .optionalChain:
          break
        default:
          description.append(".")
        }
        switch rawComponent.value {
        case .class(let offset),
            .struct(let offset):
          let count = _getRecursiveChildCount(valueType)
          let index = (0..<count)
            .first(where: { i in
              _getChildOffset(
                valueType,
                index: i
              ) == offset
            })
          if let index = index {
            var field = _FieldReflectionMetadata()
            _ = _getChildMetadata(
              valueType,
              index: index,
              fieldMetadata: &field
            )
            defer {
              field.freeFunc?(field.name)
            }
            description.append(String(cString: field.name))
          } else {
            description.append("<offset \(offset) (\(nextType))>")
          }
        case .get(_, let accessors, _),
            .nonmutatingGetSet(_, let accessors, _),
            .mutatingGetSet(_, let accessors, _):
          func project<Base>(base: Base.Type) -> String {
            func project2<Leaf>(leaf: Leaf.Type) -> String {
              dynamicLibraryAddress(
                of: accessors,
                base,
                leaf
              )
            }
            return _openExistential(nextType, do: project2)
          }
          description.append(
            _openExistential(valueType, do: project)
          )
        case .optionalChain, .optionalWrap:
          description.append("?")
        case .optionalForce:
          description.append("!")
        }
        if hasEnded {
          break
        }
        valueType = nextType
      }
      return description
    }
  }
#else
  @available(SwiftStdlib 5.8, *)
  public var debugDescription: String {
    "(value cannot be printed without reflection)"
  }
#endif
  
}
