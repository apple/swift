//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

#if _runtime(_ObjC)
// Swift's String bridges NSString via this protocol and these
// variables, allowing the core stdlib to remain decoupled from
// Foundation.

/// Effectively an untyped NSString that doesn't require foundation.
public typealias _CocoaString = AnyObject

public // @testable
func _stdlib_binary_CFStringCreateCopy(
  _ source: _CocoaString
) -> _CocoaString {
  let result = _swift_stdlib_CFStringCreateCopy(nil, source) as AnyObject
  Builtin.release(result)
  return result
}

public // @testable
func _stdlib_binary_CFStringGetLength(
  _ source: _CocoaString
) -> Int {
  return _swift_stdlib_CFStringGetLength(source)
}

public // @testable
func _stdlib_binary_CFStringGetCharactersPtr(
  _ source: _CocoaString
) -> UnsafeMutablePointer<UTF16.CodeUnit>? {
  return UnsafeMutablePointer(mutating: _swift_stdlib_CFStringGetCharactersPtr(source))
}

/// Loading Foundation initializes these function variables
/// with useful values

/// Produces a `_StringBuffer` from a given subrange of a source
/// `_CocoaString`, having the given minimum capacity.
@inline(never) @_semantics("stdlib_binary_only") // Hide the CF dependency
internal func _cocoaStringToContiguous(
  source: _CocoaString, range: Range<Int>, minimumCapacity: Int
) -> _StringBuffer {
  // FIXME(eager-bridging): I don't understand what this check was really for,
  // but it seems important! Currently we're tripping it when running tests.
  // _sanityCheck(_swift_stdlib_CFStringGetCharactersPtr(source) == nil,
  //  "Known contiguously stored strings should already be converted to Swift")

  let startIndex = range.lowerBound
  let count = range.upperBound - startIndex

  let buffer = _StringBuffer(capacity: max(count, minimumCapacity), 
                             initialSize: count, elementWidth: 2)

  _swift_stdlib_CFStringGetCharacters(
    source, _swift_shims_CFRange(location: startIndex, length: count), 
    buffer.start.assumingMemoryBound(to: _swift_shims_UniChar.self))
  
  return buffer
}

/// Reads the entire contents of a _CocoaString into contiguous
/// storage of sufficient capacity.
@inline(never) @_semantics("stdlib_binary_only") // Hide the CF dependency
internal func _cocoaStringReadAll(
  _ source: _CocoaString, _ destination: UnsafeMutablePointer<UTF16.CodeUnit>
) {
  _swift_stdlib_CFStringGetCharacters(
    source, _swift_shims_CFRange(
      location: 0, length: _swift_stdlib_CFStringGetLength(source)), destination)
}

//
// Conversion from NSString to Swift's native representation
//

internal var kCFStringEncodingASCII : _swift_shims_CFStringEncoding {
  return 0x0600
}

extension String {
  @inline(never) @_semantics("stdlib_binary_only") // Hide the CF dependency
  public // SPI(Foundation)
  init(_cocoaString: AnyObject) {
    if let wrapped = _cocoaString as? _NSContiguousString {
      self._core = wrapped._core
      return
    }

    // "copy" it into a value to be sure nobody will modify behind
    // our backs.  In practice, when value is already immutable, this
    // just does a retain.
    let cfImmutableValue
      = _stdlib_binary_CFStringCreateCopy(_cocoaString) as AnyObject

    let length = _swift_stdlib_CFStringGetLength(cfImmutableValue)

    self._core = _StringCore(
      _cocoaStringToContiguous(
        source: cfImmutableValue, range: 0..<length, minimumCapacity: 0))
  }
}

// At runtime, this class is derived from `_SwiftNativeNSStringBase`,
// which is derived from `NSString`.
//
// The @_swift_native_objc_runtime_base attribute
// This allows us to subclass an Objective-C class and use the fast Swift
// memory allocator.
@objc @_swift_native_objc_runtime_base(_SwiftNativeNSStringBase)
public class _SwiftNativeNSString {}

@objc
public protocol _NSStringCore :
    _NSCopying, _NSFastEnumeration {

  // The following methods should be overridden when implementing an
  // NSString subclass.

  func length() -> Int

  func characterAtIndex(_ index: Int) -> UInt16

  // We also override the following methods for efficiency.
}

/// An `NSString` built around a slice of contiguous Swift `String` storage.
public final class _NSContiguousString : _SwiftNativeNSString {
  public init(_ _core: _StringCore) {
    self._core = _core
    super.init()
  }

  init(coder aDecoder: AnyObject) {
    _sanityCheckFailure("init(coder:) not implemented for _NSContiguousString")
  }

  func length() -> Int {
    return _core.count
  }

  func characterAtIndex(_ index: Int) -> UInt16 {
    return _core[index]
  }

  @inline(__always) // Performance: To save on reference count operations.
  func getCharacters(
    _ buffer: UnsafeMutablePointer<UInt16>,
    range aRange: _SwiftNSRange) {

    _precondition(aRange.location + aRange.length <= Int(_core.count))

    if _core.elementWidth == 2 {
      UTF16._copy(
        source: _core.startUTF16 + aRange.location,
        destination: UnsafeMutablePointer<UInt16>(buffer),
        count: aRange.length)
    }
    else {
      UTF16._copy(
        source: _core.startASCII + aRange.location,
        destination: UnsafeMutablePointer<UInt16>(buffer),
        count: aRange.length)
    }
  }

  @objc
  func _fastCharacterContents() -> UnsafeMutablePointer<UInt16>? {
    return _core.elementWidth == 2 ? _core.startUTF16 : nil
  }

  //
  // Implement sub-slicing without adding layers of wrapping
  //
  func substringFromIndex(_ start: Int) -> _NSContiguousString {
    return _NSContiguousString(_core[Int(start)..<Int(_core.count)])
  }

  func substringToIndex(_ end: Int) -> _NSContiguousString {
    return _NSContiguousString(_core[0..<Int(end)])
  }

  func substringWithRange(_ aRange: _SwiftNSRange) -> _NSContiguousString {
    return _NSContiguousString(
      _core[Int(aRange.location)..<Int(aRange.location + aRange.length)])
  }

  func copy() -> AnyObject {
    // Since this string is immutable we can just return ourselves.
    return self
  }

  /// The caller of this function guarantees that the closure 'body' does not
  /// escape the object referenced by the opaque pointer passed to it or
  /// anything transitively reachable form this object. Doing so
  /// will result in undefined behavior.
  @_semantics("self_no_escaping_closure")
  func _unsafeWithNotEscapedSelfPointer<Result>(
    _ body: (OpaquePointer) throws -> Result
  ) rethrows -> Result {
    let selfAsPointer = unsafeBitCast(self, to: OpaquePointer.self)
    defer {
      _fixLifetime(self)
    }
    return try body(selfAsPointer)
  }

  /// The caller of this function guarantees that the closure 'body' does not
  /// escape either object referenced by the opaque pointer pair passed to it or
  /// transitively reachable objects. Doing so will result in undefined
  /// behavior.
  @_semantics("pair_no_escaping_closure")
  func _unsafeWithNotEscapedSelfPointerPair<Result>(
    _ rhs: _NSContiguousString,
    _ body: (OpaquePointer, OpaquePointer) throws -> Result
  ) rethrows -> Result {
    let selfAsPointer = unsafeBitCast(self, to: OpaquePointer.self)
    let rhsAsPointer = unsafeBitCast(rhs, to: OpaquePointer.self)
    defer {
      _fixLifetime(self)
      _fixLifetime(rhs)
    }
    return try body(selfAsPointer, rhsAsPointer)
  }

  public let _core: _StringCore
}

extension String {
  /// Same as `_bridgeToObjectiveC()`, but located inside the core standard
  /// library.
  public func _stdlib_binary_bridgeToObjectiveCImpl() -> AnyObject {
    return _NSContiguousString(_core)
  }

  @inline(never) @_semantics("stdlib_binary_only") // Hide the CF dependency
  public func _bridgeToObjectiveCImpl() -> AnyObject {
    return _stdlib_binary_bridgeToObjectiveCImpl()
  }
}
#endif
