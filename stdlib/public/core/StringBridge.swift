//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

/// Effectively an untyped NSString that doesn't require foundation.
@usableFromInline
internal typealias _CocoaString = AnyObject

#if _runtime(_ObjC)

// Swift's String bridges NSString via this protocol and these
// variables, allowing the core stdlib to remain decoupled from
// Foundation.

@objc private protocol _StringSelectorHolder : _NSCopying {

  @objc var length: Int { get }

  @objc var hash: UInt { get }

  @objc(characterAtIndex:)
  func character(at offset: Int) -> UInt16

  @objc(getCharacters:range:)
  func getCharacters(
   _ buffer: UnsafeMutablePointer<UInt16>, range aRange: _SwiftNSRange
  )

  @objc(_fastCStringContents:)
  func _fastCStringContents(
    _ requiresNulTermination: Int8
  ) -> UnsafePointer<CChar>?

  @objc(_fastCharacterContents)
  func _fastCharacterContents() -> UnsafePointer<UInt16>?

  @objc(getBytes:maxLength:usedLength:encoding:options:range:remainingRange:)
  func getBytes(_ buffer: UnsafeMutableRawPointer?,
   maxLength maxBufferCount: Int,
  usedLength usedBufferCount: UnsafeMutablePointer<Int>?,
    encoding: UInt,
     options: UInt,
       range: _SwiftNSRange,
       remaining leftover: UnsafeMutablePointer<_SwiftNSRange>?) -> Int8

  @objc(compare:options:range:locale:)
  func compare(_ string: _CocoaString,
               options: UInt,
               range: _SwiftNSRange,
               locale: AnyObject?) -> Int

  @objc(newTaggedNSStringWithASCIIBytes_:length_:)
  func createTaggedString(bytes: UnsafePointer<UInt8>,
                          count: Int) -> AnyObject?
}

/*
 Passing a _CocoaString through _objc() lets you call ObjC methods that the
 compiler doesn't know about, via the protocol above. In order to get good
 performance, you need a double indirection like this:

  func a -> _objc -> func a'

 because any refcounting @_effects on 'a' will be lost when _objc breaks ARC's
 knowledge that the _CocoaString and _StringSelectorHolder are the same object.
 */
@inline(__always)
private func _objc(_ str: _CocoaString) -> _StringSelectorHolder {
  return unsafeBitCast(str, to: _StringSelectorHolder.self)
}

@_effects(releasenone)
private func _copyNSString(_ str: _StringSelectorHolder) -> _CocoaString {
  return str.copy(with: nil)
}

@usableFromInline // @testable
@_effects(releasenone)
internal func _stdlib_binary_CFStringCreateCopy(
  _ source: _CocoaString
) -> _CocoaString {
  return _copyNSString(_objc(source))
}

@_effects(readonly)
private func _NSStringLen(_ str: _StringSelectorHolder) -> Int {
  return str.length
}

@usableFromInline // @testable
@_effects(readonly)
internal func _stdlib_binary_CFStringGetLength(
  _ source: _CocoaString
) -> Int {
  if let len = getConstantTaggedCocoaContents(source)?.utf16Length {
    return len
  }
  return _NSStringLen(_objc(source))
}

@_effects(readonly)
internal func _isNSString(_ str:AnyObject) -> Bool {
  if getConstantTaggedCocoaContents(str) != nil {
    return true
  }
  return _swift_stdlib_isNSString(str) != 0
}

@_effects(readonly)
private func _NSStringCharactersPtr(_ str: _StringSelectorHolder) -> UnsafeMutablePointer<UTF16.CodeUnit>? {
  return UnsafeMutablePointer(mutating: str._fastCharacterContents())
}

@usableFromInline // @testable
@_effects(readonly)
internal func _stdlib_binary_CFStringGetCharactersPtr(
  _ source: _CocoaString
) -> UnsafeMutablePointer<UTF16.CodeUnit>? {
  return _NSStringCharactersPtr(_objc(source))
}

@_effects(releasenone)
private func _NSStringGetCharacters(
  from source: _StringSelectorHolder,
  range: Range<Int>,
  into destination: UnsafeMutablePointer<UTF16.CodeUnit>
) {
  source.getCharacters(destination, range: _SwiftNSRange(
    location: range.startIndex,
    length: range.count)
  )
}

/// Copies a slice of a _CocoaString into contiguous storage of sufficient
/// capacity.
@_effects(releasenone)
internal func _cocoaStringCopyCharacters(
  from source: _CocoaString,
  range: Range<Int>,
  into destination: UnsafeMutablePointer<UTF16.CodeUnit>
) {
  _NSStringGetCharacters(from: _objc(source), range: range, into: destination)
}

@_effects(readonly)
private func _NSStringGetCharacter(
  _ target: _StringSelectorHolder, _ position: Int
) -> UTF16.CodeUnit {
  return target.character(at: position)
}

@_effects(readonly)
internal func _cocoaStringSubscript(
  _ target: _CocoaString, _ position: Int
) -> UTF16.CodeUnit {
  return _NSStringGetCharacter(_objc(target), position)
}

@_effects(releasenone)
private func _NSStringCopyBytes(
  _ o: _StringSelectorHolder,
  encoding: UInt,
  into bufPtr: UnsafeMutableRawBufferPointer
) -> Int? {
  let ptr = bufPtr.baseAddress._unsafelyUnwrappedUnchecked
  let len = o.length
  var remainingRange = _SwiftNSRange(location: 0, length: 0)
  var usedLen = 0
  let success = 0 != o.getBytes(
    ptr,
    maxLength: bufPtr.count,
    usedLength: &usedLen,
    encoding: encoding,
    options: 0,
    range: _SwiftNSRange(location: 0, length: len),
    remaining: &remainingRange
  )
  if success && remainingRange.length == 0 {
    return usedLen
  }
  return nil
}

@_effects(releasenone)
internal func _cocoaStringCopyUTF8(
  _ target: _CocoaString,
  into bufPtr: UnsafeMutableRawBufferPointer
) -> Int? {
  return _NSStringCopyBytes(
    _objc(target),
    encoding: _cocoaUTF8Encoding,
    into: bufPtr
  )
}

@_effects(releasenone)
internal func _cocoaStringCopyASCII(
  _ target: _CocoaString,
  into bufPtr: UnsafeMutableRawBufferPointer
) -> Int? {
  return _NSStringCopyBytes(
    _objc(target),
    encoding: _cocoaASCIIEncoding,
    into: bufPtr
  )
}

@_effects(readonly)
private func _NSStringUTF8Count(
  _ o: _StringSelectorHolder,
  range: Range<Int>
) -> Int? {
  var remainingRange = _SwiftNSRange(location: 0, length: 0)
  var usedLen = 0
  let success = 0 != o.getBytes(
    UnsafeMutableRawPointer(Builtin.inttoptr_Word(0._builtinWordValue)),
    maxLength: 0,
    usedLength: &usedLen,
    encoding: _cocoaUTF8Encoding,
    options: 0,
    range: _SwiftNSRange(location: range.startIndex, length: range.count),
    remaining: &remainingRange
  )
  if success && remainingRange.length == 0 {
    return usedLen
  }
  return nil
}

@_effects(readonly)
internal func _cocoaStringUTF8Count(
  _ target: _CocoaString,
  range: Range<Int>
) -> Int? {
  if range.isEmpty { return 0 }
  return _NSStringUTF8Count(_objc(target), range: range)
}

@_effects(readonly)
private func _NSStringCompare(
  _ o: _StringSelectorHolder, _ other: _CocoaString
) -> Int {
  let range = _SwiftNSRange(location: 0, length: o.length)
  let options = UInt(2) /* NSLiteralSearch*/
  return o.compare(other, options: options, range: range, locale: nil)
}

@_effects(readonly)
internal func _cocoaStringCompare(
  _ string: _CocoaString, _ other: _CocoaString
) -> Int {
  return _NSStringCompare(_objc(string), other)
}

@_effects(readonly)
internal func _cocoaHashString(
  _ string: _CocoaString
) -> UInt {
  return _swift_stdlib_CFStringHashNSString(string)
}

@_effects(readonly)
internal func _cocoaHashASCIIBytes(
  _ bytes: UnsafePointer<UInt8>, length: Int
) -> UInt {
  return _swift_stdlib_CFStringHashCString(bytes, length)
}

// These "trampolines" are effectively objc_msgSend_super.
// They bypass our implementations to use NSString's.

@_effects(readonly)
internal func _cocoaCStringUsingEncodingTrampoline(
  _ string: _CocoaString, _ encoding: UInt
) -> UnsafePointer<UInt8>? {
  return _swift_stdlib_NSStringCStringUsingEncodingTrampoline(string, encoding)
}

@_effects(releasenone)
internal func _cocoaGetCStringTrampoline(
  _ string: _CocoaString,
  _ buffer: UnsafeMutablePointer<UInt8>,
  _ maxLength: Int,
  _ encoding: UInt
) -> Int8 {
  return Int8(_swift_stdlib_NSStringGetCStringTrampoline(
    string, buffer, maxLength, encoding))
}

//
// Conversion from NSString to Swift's native representation.
//

private var kCFStringEncodingASCII: _swift_shims_CFStringEncoding {
  @inline(__always) get { return 0x0600 }
}

private var kCFStringEncodingUTF8: _swift_shims_CFStringEncoding {
  @inline(__always) get { return 0x8000100 }
}

internal enum _KnownCocoaString {
  case storage
  case shared
  case cocoa
#if _pointerBitWidth(_64)
  case tagged
#endif
#if arch(arm64)
  case constantTagged
#endif

  @inline(__always)
  init(_ str: _CocoaString) {

#if _pointerBitWidth(_64)
    if _isObjCTaggedPointer(str) {
#if arch(arm64)
      if let _ = getConstantTaggedCocoaContents(str) {
        self = .constantTagged
      } else {
        self = .tagged
      }
#else
      self = .tagged
#endif
      return
    }
#endif

    switch unsafeBitCast(_swift_classOfObjCHeapObject(str), to: UInt.self) {
    case unsafeBitCast(__StringStorage.self, to: UInt.self):
      self = .storage
    case unsafeBitCast(__SharedStringStorage.self, to: UInt.self):
      self = .shared
    default:
      self = .cocoa
    }
  }
}

#if _pointerBitWidth(_64)
// Resiliently write a tagged _CocoaString's contents into a buffer.
// The Foundation overlay takes care of bridging tagged pointer strings before
// they reach us, but this may still be called by older code, or by strings
// entering our domain via the arguments to -isEqual:, etc...
@_effects(releasenone) // @opaque
internal func _bridgeTagged(
  _ cocoa: _CocoaString,
  intoUTF8 bufPtr: UnsafeMutableRawBufferPointer
) -> Int? {
  _internalInvariant(_isObjCTaggedPointer(cocoa))
  return _cocoaStringCopyUTF8(cocoa, into: bufPtr)
}

@_effects(releasenone) // @opaque
internal func _bridgeTaggedASCII(
  _ cocoa: _CocoaString,
  intoUTF8 bufPtr: UnsafeMutableRawBufferPointer
) -> Int? {
  _internalInvariant(_isObjCTaggedPointer(cocoa))
  return _cocoaStringCopyASCII(cocoa, into: bufPtr)
}
#endif

@_effects(readonly)
private func _NSStringASCIIPointer(_ str: _StringSelectorHolder) -> UnsafePointer<UInt8>? {
  //TODO(String bridging): Unconditionally asking for nul-terminated contents is
  // overly conservative and hurts perf with some NSStrings
  return str._fastCStringContents(1)?._asUInt8
}

@_effects(readonly)
private func _NSStringUTF8Pointer(_ str: _StringSelectorHolder) -> UnsafePointer<UInt8>? {
  //We don't have a way to ask for UTF8 here currently
  return _NSStringASCIIPointer(str)
}

@_effects(readonly) // @opaque
private func _withCocoaASCIIPointer<R>(
  _ str: _CocoaString,
  requireStableAddress: Bool,
  work: (UnsafePointer<UInt8>) -> R?
) -> R? {
  #if _pointerBitWidth(_64)
  if _isObjCTaggedPointer(str) {
    if let ptr = getConstantTaggedCocoaContents(str)?.asciiContentsPointer {
      return work(ptr)
    }
    if requireStableAddress {
      return nil // tagged pointer strings don't support _fastCStringContents
    }
    if let smol = _SmallString(taggedASCIICocoa: str) {
      return _StringGuts(smol).withFastUTF8 {
        work($0.baseAddress._unsafelyUnwrappedUnchecked)
      }
    }
  }
  #endif
  defer { _fixLifetime(str) }
  if let ptr = _NSStringASCIIPointer(_objc(str)) {
    return work(ptr)
  }
  return nil
}

@_effects(readonly) // @opaque
private func _withCocoaUTF8Pointer<R>(
  _ str: _CocoaString,
  requireStableAddress: Bool,
  work: (UnsafePointer<UInt8>) -> R?
) -> R? {
  #if _pointerBitWidth(_64)
  if _isObjCTaggedPointer(str) {
    if let ptr = getConstantTaggedCocoaContents(str)?.asciiContentsPointer {
      return work(ptr)
    }
    if requireStableAddress {
      return nil // tagged pointer strings don't support _fastCStringContents
    }
    if let smol = _SmallString(taggedCocoa: str) {
      return _StringGuts(smol).withFastUTF8 {
        work($0.baseAddress._unsafelyUnwrappedUnchecked)
      }
    }
  }
  #endif
  defer { _fixLifetime(str) }
  if let ptr = _NSStringUTF8Pointer(_objc(str)) {
    return work(ptr)
  }
  return nil
}

@_effects(readonly) // @opaque
internal func withCocoaASCIIPointer<R>(
  _ str: _CocoaString,
  work: (UnsafePointer<UInt8>) -> R?
) -> R? {
  return _withCocoaASCIIPointer(str, requireStableAddress: false, work: work)
}

@_effects(readonly) // @opaque
internal func withCocoaUTF8Pointer<R>(
  _ str: _CocoaString,
  work: (UnsafePointer<UInt8>) -> R?
) -> R? {
  return _withCocoaUTF8Pointer(str, requireStableAddress: false, work: work)
}

@_effects(readonly)
internal func stableCocoaASCIIPointer(_ str: _CocoaString)
  -> UnsafePointer<UInt8>? {
  return _withCocoaASCIIPointer(str, requireStableAddress: true, work: { $0 })
}

@_effects(readonly)
internal func stableCocoaUTF8Pointer(_ str: _CocoaString)
  -> UnsafePointer<UInt8>? {
  return _withCocoaUTF8Pointer(str, requireStableAddress: true, work: { $0 })
}

private enum CocoaStringPointer {
  case ascii(UnsafePointer<UInt8>)
  case utf8(UnsafePointer<UInt8>)
  case utf16(UnsafePointer<UInt16>)
  case none
}

@_effects(readonly)
private func _getCocoaStringPointer(
  _ cfImmutableValue: _CocoaString
) -> CocoaStringPointer {
  if let ascii = stableCocoaASCIIPointer(cfImmutableValue) {
    return .ascii(ascii)
  }
  // We could ask for UTF16 here via _stdlib_binary_CFStringGetCharactersPtr,
  // but we currently have no use for it
  return .none
}

#if arch(arm64)
//11000000..payload..111
private var constantTagMask:UInt {
  0b1111_1111_1000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0111
}
private var expectedConstantTagValue:UInt {
  0b1100_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0000_0111
}
#endif

@inline(__always)
private func formConstantTaggedCocoaString(
  untaggedCocoa: _CocoaString
) -> AnyObject? {
#if !arch(arm64)
  return nil
#else

  let constantPtr:UnsafeRawPointer = Builtin.reinterpretCast(untaggedCocoa)

  // Check if what we're pointing to is actually a valid tagged constant
  guard _swift_stdlib_dyld_is_objc_constant_string(constantPtr) == 1 else {
    return nil
  }

  let retaggedPointer = UInt(bitPattern: constantPtr) | expectedConstantTagValue

  return unsafeBitCast(retaggedPointer, to: AnyObject.self)
#endif
}

@inline(__always)
private func getConstantTaggedCocoaContents(_ cocoaString: _CocoaString) ->
    (utf16Length: Int,
     asciiContentsPointer: UnsafePointer<UInt8>,
     untaggedCocoa: _CocoaString)? {
#if !arch(arm64)
  return nil
#else

  guard _isObjCTaggedPointer(cocoaString) else {
    return nil
  }

  let taggedValue = unsafeBitCast(cocoaString, to: UInt.self)
  


  guard taggedValue & constantTagMask == expectedConstantTagValue else {
    return nil
  }

  let payloadMask = ~constantTagMask
  let payload = taggedValue & payloadMask
  let ivarPointer = UnsafePointer<_swift_shims_builtin_CFString>(
    bitPattern: payload
  )!

  guard _swift_stdlib_dyld_is_objc_constant_string(
    UnsafeRawPointer(ivarPointer)
  ) == 1 else {
    return nil
  }

  let length = ivarPointer.pointee.length
  let isUTF16Mask:UInt = 0x0000_0000_0000_0004 //CFStringFlags bit 4: isUnicode
  let isASCII = ivarPointer.pointee.flags & isUTF16Mask == 0
  _precondition(isASCII) // we don't currently support non-ASCII here
  let contentsPtr = ivarPointer.pointee.str
  return (
    utf16Length: Int(length),
    asciiContentsPointer: contentsPtr,
    untaggedCocoa: Builtin.reinterpretCast(ivarPointer)
  )
#endif
}

@usableFromInline
@_effects(releasenone) // @opaque
internal func _bridgeCocoaString(_ cocoaString: _CocoaString) -> _StringGuts {
  switch _KnownCocoaString(cocoaString) {
  case .storage:
    return _unsafeUncheckedDowncast(
      cocoaString, to: __StringStorage.self).asString._guts
  case .shared:
    return _unsafeUncheckedDowncast(
      cocoaString, to: __SharedStringStorage.self).asString._guts
#if _pointerBitWidth(_64)
  case .tagged:
    // Foundation should be taking care of tagged pointer strings before they
    // reach here, so the only ones reaching this point should be back deployed,
    // which will never have tagged pointer strings that aren't small, hence
    // the force unwrap here.
    return _StringGuts(_SmallString(taggedCocoa: cocoaString)!)
#if arch(arm64)
  case .constantTagged:
    let taggedContents = getConstantTaggedCocoaContents(cocoaString)!
    return _StringGuts(
      cocoa: taggedContents.untaggedCocoa,
      providesFastUTF8: false, //TODO: if contentsPtr is UTF8 compatible, use it
      isASCII: true,
      length: taggedContents.utf16Length
    )
#endif
#endif
  case .cocoa:
    // "Copy" it into a value to be sure nobody will modify behind
    // our backs. In practice, when value is already immutable, this
    // just does a retain.
    //
    // TODO: Only in certain circumstances should we emit this call:
    //   1) If it's immutable, just retain it.
    //   2) If it's mutable with no associated information, then a copy must
    //      happen; might as well eagerly bridge it in.
    //   3) If it's mutable with associated information, must make the call
    let immutableCopy
      = _stdlib_binary_CFStringCreateCopy(cocoaString)

#if _pointerBitWidth(_64)
    if _isObjCTaggedPointer(immutableCopy) {
      // Copying a tagged pointer can produce a tagged pointer, but only if it's
      // small enough to definitely fit in a _SmallString
      return _StringGuts(
        _SmallString(taggedCocoa: immutableCopy).unsafelyUnwrapped
      )
    }
#endif

    let (fastUTF8, isASCII): (Bool, Bool)
    switch _getCocoaStringPointer(immutableCopy) {
    case .ascii(_): (fastUTF8, isASCII) = (true, true)
    case .utf8(_): (fastUTF8, isASCII) = (true, false)
    default:  (fastUTF8, isASCII) = (false, false)
    }
    let length = _stdlib_binary_CFStringGetLength(immutableCopy)

    return _StringGuts(
      cocoa: immutableCopy,
      providesFastUTF8: fastUTF8,
      isASCII: isASCII,
      length: length)
  }
}

extension String {
  @_spi(Foundation)
  public init(_cocoaString: AnyObject) {
    self._guts = _bridgeCocoaString(_cocoaString)
  }
}

@_effects(releasenone)
private func _createNSString(
  _ receiver: _StringSelectorHolder,
  _ ptr: UnsafePointer<UInt8>,
  _ count: Int,
  _ encoding: UInt32
) -> AnyObject? {
  return receiver.createTaggedString(bytes: ptr, count: count)
}

@_effects(releasenone)
private func _createCFString(
  _ ptr: UnsafePointer<UInt8>,
  _ count: Int,
  _ encoding: UInt32
) -> AnyObject? {
  return _createNSString(
    unsafeBitCast(__StringStorage.self as AnyClass, to: _StringSelectorHolder.self),
    ptr,
    count,
    encoding
  )
}

extension String {
  @_effects(releasenone)
  public // SPI(Foundation)
  func _bridgeToObjectiveCImpl() -> AnyObject {

    _connectOrphanedFoundationSubclassesIfNeeded()

    // Smol ASCII a) may bridge to tagged pointers, b) can't contain a BOM
    if _guts.isSmallASCII {
      let maybeTagged = _guts.asSmall.withUTF8 { bufPtr in
        return _createCFString(
          bufPtr.baseAddress._unsafelyUnwrappedUnchecked,
          bufPtr.count,
          kCFStringEncodingUTF8
        )
      }
      if let tagged = maybeTagged { return tagged }
    }

    if _guts.isSmall {
        // We can't form a tagged pointer String, so grow to a non-small String,
        // and bridge that instead. Also avoids CF deleting any BOM that may be
        // present
        var copy = self
        // TODO: small capacity minimum is lifted, just need to make native
        copy._guts.grow(_SmallString.capacity + 1)
        _internalInvariant(!copy._guts.isSmall)
        return copy._bridgeToObjectiveCImpl()
    }
    if _guts._object.isImmortal {
      // TODO: We'd rather emit a valid ObjC object statically than create a
      // shared string class instance.
      let gutsCountAndFlags = _guts._object._countAndFlags
      return __SharedStringStorage(
        immortal: _guts._object.fastUTF8.baseAddress!,
        countAndFlags: _StringObject.CountAndFlags(
          sharedCount: _guts.count, isASCII: gutsCountAndFlags.isASCII))
    }

    _internalInvariant(_guts._object.hasObjCBridgeableObject,
      "Unknown non-bridgeable object case")
    let result = _guts._object.objCBridgeableObject
    return formConstantTaggedCocoaString(untaggedCocoa: result) ?? result
  }
}

// Note: This function is not intended to be called from Swift.  The
// availability information here is perfunctory; this function isn't considered
// part of the Stdlib's Swift ABI.
@available(SwiftStdlib 5.2, *)
@_cdecl("_SwiftCreateBridgedString")
@usableFromInline
internal func _SwiftCreateBridgedString_DoNotCall(
  bytes: UnsafePointer<UInt8>,
  length: Int,
  encoding: _swift_shims_CFStringEncoding
) -> Unmanaged<AnyObject> {
  let bufPtr = UnsafeBufferPointer(start: bytes, count: length)
  let str:String
  switch encoding {
  case kCFStringEncodingUTF8:
    str = String(decoding: bufPtr, as: Unicode.UTF8.self)
  case kCFStringEncodingASCII:
    str = String(decoding: bufPtr, as: Unicode.ASCII.self)
  default:
    fatalError("Unsupported encoding in shim")
  }
  return Unmanaged<AnyObject>.passRetained(str._bridgeToObjectiveCImpl())
}

// At runtime, this class is derived from `__SwiftNativeNSStringBase`,
// which is derived from `NSString`.
//
// The @_swift_native_objc_runtime_base attribute
// This allows us to subclass an Objective-C class and use the fast Swift
// memory allocator.
@objc @_swift_native_objc_runtime_base(__SwiftNativeNSStringBase)
@_spi(Foundation) public class __SwiftNativeNSString {
  @objc internal init() {}
  deinit {}
}

// Called by the SwiftObject implementation to get the description of a value
// as an NSString.
@_silgen_name("swift_stdlib_getDescription")
public func _getDescription<T>(_ x: T) -> AnyObject {
  return String(reflecting: x)._bridgeToObjectiveCImpl()
}

@_silgen_name("swift_stdlib_NSStringFromUTF8")
@usableFromInline //this makes the symbol available to the runtime :(
@available(SwiftStdlib 5.2, *)
internal func _NSStringFromUTF8(_ s: UnsafePointer<UInt8>, _ len: Int)
  -> AnyObject {
  return String(
    decoding: UnsafeBufferPointer(start: s, count: len),
    as: UTF8.self
  )._bridgeToObjectiveCImpl()
}

#else // !_runtime(_ObjC)

internal class __SwiftNativeNSString {
  internal init() {}
  deinit {}
}

#endif

// Special-case Index <-> Offset converters for bridging and use in accelerating
// the UTF16View in general.
extension StringProtocol {
  @_specialize(where Self == String)
  @_specialize(where Self == Substring)
  public // SPI(Foundation)
  func _toUTF16Offset(_ idx: Index) -> Int {
    return self.utf16.distance(from: self.utf16.startIndex, to: idx)
  }

  @_specialize(where Self == String)
  @_specialize(where Self == Substring)
  public // SPI(Foundation)
  func _toUTF16Index(_ offset: Int) -> Index {
    return self.utf16.index(self.utf16.startIndex, offsetBy: offset)
  }

  public // SPI(Foundation)
  func _toUTF16Offsets(_ indices: Range<Index>) -> Range<Int> {
    if Self.self == String.self {
      let s = unsafeBitCast(self, to: String.self)
      return s.utf16._offsetRange(for: indices, from: s.startIndex)
    }
    if Self.self == Substring.self {
      let s = unsafeBitCast(self, to: Substring.self)
      return s._slice._base.utf16._offsetRange(for: indices, from: s.startIndex)
    }
    let startOffset = _toUTF16Offset(indices.lowerBound)
    let endOffset = _toUTF16Offset(indices.upperBound)
    return Range(uncheckedBounds: (lower: startOffset, upper: endOffset))
  }

  public // SPI(Foundation)
  func _toUTF16Indices(_ range: Range<Int>) -> Range<Index> {
    if Self.self == String.self {
      let s = unsafeBitCast(self, to: String.self)
      return s.utf16._indexRange(for: range, from: s.startIndex)
    }
    if Self.self == Substring.self {
      let s = unsafeBitCast(self, to: Substring.self)
      return s._slice._base.utf16._indexRange(for: range, from: s.startIndex)
    }
    let lowerbound = _toUTF16Index(range.lowerBound)
    let upperbound = _toUTF16Index(range.upperBound)
    return Range(uncheckedBounds: (lower: lowerbound, upper: upperbound))
  }
}

extension String {
  public // @testable / @benchmarkable
  func _copyUTF16CodeUnits(
    into buffer: UnsafeMutableBufferPointer<UInt16>,
    range: Range<Int>
  ) {
    _internalInvariant(buffer.count >= range.count)
    let indexRange = self._toUTF16Indices(range)
    self.utf16._nativeCopy(into: buffer, alignedRange: indexRange)
  }
}
