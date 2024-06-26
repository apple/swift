//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// String Creation Helpers
//===----------------------------------------------------------------------===//

internal func _allASCII(_ input: UnsafeBufferPointer<UInt8>) -> Bool {
  if input.isEmpty { return true }

  // NOTE: Avoiding for-in syntax to avoid bounds checks
  //
  // TODO(String performance): SIMD-ize
  //
  let count = input.count
  var ptr = UnsafeRawPointer(input.baseAddress._unsafelyUnwrappedUnchecked)

  let asciiMask64 = 0x8080_8080_8080_8080 as UInt64
  let asciiMask32 = UInt32(truncatingIfNeeded: asciiMask64)
  let asciiMask16 = UInt16(truncatingIfNeeded: asciiMask64)
  let asciiMask8 = UInt8(truncatingIfNeeded: asciiMask64)
  
  let end128 = ptr + count & ~(MemoryLayout<(UInt64, UInt64)>.stride &- 1)
  let end64 = ptr + count & ~(MemoryLayout<UInt64>.stride &- 1)
  let end32 = ptr + count & ~(MemoryLayout<UInt32>.stride &- 1)
  let end16 = ptr + count & ~(MemoryLayout<UInt16>.stride &- 1)
  let end = ptr + count

  
  while ptr < end128 {
    let pair = ptr.loadUnaligned(as: (UInt64, UInt64).self)
    let result = (pair.0 | pair.1) & asciiMask64
    guard result == 0 else { return false }
    ptr = ptr + MemoryLayout<(UInt64, UInt64)>.stride
  }
  
  // If we had enough bytes for two iterations of this, we would have hit
  // the loop above, so we only need to do this once
  if ptr < end64 {
    let value = ptr.loadUnaligned(as: UInt64.self)
    guard value & asciiMask64 == 0 else { return false }
    ptr = ptr + MemoryLayout<UInt64>.stride
  }
  
  if ptr < end32 {
    let value = ptr.loadUnaligned(as: UInt32.self)
    guard value & asciiMask32 == 0 else { return false }
    ptr = ptr + MemoryLayout<UInt32>.stride
  }
  
  if ptr < end16 {
    let value = ptr.loadUnaligned(as: UInt16.self)
    guard value & asciiMask16 == 0 else { return false }
    ptr = ptr + MemoryLayout<UInt16>.stride
  }

  if ptr < end {
    let value = ptr.loadUnaligned(fromByteOffset: 0, as: UInt8.self)
    guard value & asciiMask8 == 0 else { return false }
  }
  _internalInvariant(ptr == end || ptr + 1 == end)
  return true
}

extension String {

  internal static func _uncheckedFromASCII(
    _ input: UnsafeBufferPointer<UInt8>
  ) -> String {
    if let smol = _SmallString(input) {
      return String(_StringGuts(smol))
    }

    let storage = __StringStorage.create(initializingFrom: input, isASCII: true)
    return storage.asString
  }

  @usableFromInline
  internal static func _fromASCII(
    _ input: UnsafeBufferPointer<UInt8>
  ) -> String {
    _internalInvariant(_allASCII(input), "not actually ASCII")
    return _uncheckedFromASCII(input)
  }

  internal static func _fromASCIIValidating(
    _ input: UnsafeBufferPointer<UInt8>
  ) -> String? {
    if _fastPath(_allASCII(input)) {
      return _uncheckedFromASCII(input)
    }
    return nil
  }

  public // SPI(Foundation)
  static func _tryFromUTF8(_ input: UnsafeBufferPointer<UInt8>) -> String? {
    guard case .success(let extraInfo) = validateUTF8(input) else {
      return nil
    }

    return String._uncheckedFromUTF8(input, isASCII: extraInfo.isASCII)
  }

  @usableFromInline
  internal static func _fromUTF8Repairing(
    _ input: UnsafeBufferPointer<UInt8>
  ) -> (result: String, repairsMade: Bool) {
    switch validateUTF8(input) {
    case .success(let extraInfo):
      return (String._uncheckedFromUTF8(
        input, asciiPreScanResult: extraInfo.isASCII
      ), false)
    case .error(let initialRange):
        return (repairUTF8(input, firstKnownBrokenRange: initialRange), true)
    }
  }

  internal static func _fromLargeUTF8Repairing(
    uninitializedCapacity capacity: Int,
    initializingWith initializer: (
      _ buffer: UnsafeMutableBufferPointer<UInt8>
    ) throws -> Int
  ) rethrows -> String {
    let result = try __StringStorage.create(
      uninitializedCodeUnitCapacity: capacity,
      initializingUncheckedUTF8With: initializer)

    switch validateUTF8(result.codeUnits) {
    case .success(let info):
      result._updateCountAndFlags(
        newCount: result.count,
        newIsASCII: info.isASCII
      )
      return result.asString
    case .error(let initialRange):
      defer { _fixLifetime(result) }
      //This could be optimized to use excess tail capacity
      return repairUTF8(result.codeUnits, firstKnownBrokenRange: initialRange)
    }
  }

  @usableFromInline
  internal static func _uncheckedFromUTF8(
    _ input: UnsafeBufferPointer<UInt8>
  ) -> String {
    return _uncheckedFromUTF8(input, isASCII: _allASCII(input))
  }

  @usableFromInline
  internal static func _uncheckedFromUTF8(
    _ input: UnsafeBufferPointer<UInt8>,
    isASCII: Bool
  ) -> String {
    if let smol = _SmallString(input) {
      return String(_StringGuts(smol))
    }

    let storage = __StringStorage.create(
      initializingFrom: input, isASCII: isASCII)
    return storage.asString
  }

  // If we've already pre-scanned for ASCII, just supply the result
  @usableFromInline
  internal static func _uncheckedFromUTF8(
    _ input: UnsafeBufferPointer<UInt8>, asciiPreScanResult: Bool
  ) -> String {
    if let smol = _SmallString(input) {
      return String(_StringGuts(smol))
    }

    let isASCII = asciiPreScanResult
    let storage = __StringStorage.create(
      initializingFrom: input, isASCII: isASCII)
    return storage.asString
  }

  @usableFromInline
  internal static func _uncheckedFromUTF16(
    _ input: UnsafeBufferPointer<UInt16>
  ) -> String {
    // TODO(String Performance): Attempt to form smol strings

    // TODO(String performance): Skip intermediary array, transcode directly
    // into a StringStorage space.
    var contents: [UInt8] = []
    contents.reserveCapacity(input.count)
    let repaired = transcode(
      input.makeIterator(),
      from: UTF16.self,
      to: UTF8.self,
      stoppingOnError: false,
      into: { contents.append($0) })
    _internalInvariant(!repaired, "Error present")

    return contents.withUnsafeBufferPointer { String._uncheckedFromUTF8($0) }
  }

  @inline(never) // slow path
  private static func _slowFromCodeUnits<
    Input: Collection,
    Encoding: Unicode.Encoding
  >(
    _ input: Input,
    encoding: Encoding.Type,
    repair: Bool
  ) -> (String, repairsMade: Bool)?
  where Input.Element == Encoding.CodeUnit {
    // TODO(String Performance): Attempt to form smol strings

    // TODO(String performance): Skip intermediary array, transcode directly
    // into a StringStorage space.
    var contents: [UInt8] = []
    contents.reserveCapacity(input.underestimatedCount)
    let repaired = transcode(
      input.makeIterator(),
      from: Encoding.self,
      to: UTF8.self,
      stoppingOnError: false,
      into: { contents.append($0) })
    guard repair || !repaired else { return nil }

    let str = contents.withUnsafeBufferPointer { String._uncheckedFromUTF8($0) }
    return (str, repaired)
  }

  @usableFromInline @inline(never) // can't be inlined w/out breaking ABI
  @_specialize(
    where Input == UnsafeBufferPointer<UInt8>, Encoding == Unicode.ASCII)
  @_specialize(
    where Input == Array<UInt8>, Encoding == Unicode.ASCII)
  internal static func _fromCodeUnits<
    Input: Collection,
    Encoding: Unicode.Encoding
  >(
    _ input: Input,
    encoding: Encoding.Type,
    repair: Bool
  ) -> (String, repairsMade: Bool)?
  where Input.Element == Encoding.CodeUnit {
    guard _fastPath(encoding == Unicode.ASCII.self) else {
      return _slowFromCodeUnits(input, encoding: encoding, repair: repair)
    }

    // Helper to simplify early returns
    func resultOrSlow(_ resultOpt: String?) -> (String, repairsMade: Bool)? {
      guard let result = resultOpt else {
        return _slowFromCodeUnits(input, encoding: encoding, repair: repair)
      }
      return (result, repairsMade: false)
    }

    #if !$Embedded
    // Fast path for untyped raw storage and known stdlib types
    if let contigBytes = input as? _HasContiguousBytes,
      contigBytes._providesContiguousBytesNoCopy {
      return resultOrSlow(contigBytes.withUnsafeBytes { rawBufPtr in
        let buffer = UnsafeBufferPointer(
          start: rawBufPtr.baseAddress?.assumingMemoryBound(to: UInt8.self),
          count: rawBufPtr.count)
        return String._fromASCIIValidating(buffer)
      })
    }
    #endif

    // Fast path for user-defined Collections
    if let strOpt = input.withContiguousStorageIfAvailable({
      (buffer: UnsafeBufferPointer<Input.Element>) -> String? in
      return String._fromASCIIValidating(
        UnsafeRawBufferPointer(buffer).bindMemory(to: UInt8.self))
    }) {
      return resultOrSlow(strOpt)
    }

    return resultOrSlow(Array(input).withUnsafeBufferPointer {
        let buffer = UnsafeRawBufferPointer($0).bindMemory(to: UInt8.self)
        return String._fromASCIIValidating(buffer)
      })
  }

  public // @testable
  static func _fromInvalidUTF16(
    _ utf16: UnsafeBufferPointer<UInt16>
  ) -> String {
    return String._fromCodeUnits(utf16, encoding: UTF16.self, repair: true)!.0
  }

  @usableFromInline
  internal static func _fromSubstring(
    _ substring: __shared Substring
  ) -> String {
    if substring._offsetRange == substring.base._offsetRange {
      return substring.base
    }

    return String._copying(substring)
  }

  @_alwaysEmitIntoClient
  @inline(never) // slow-path
  internal static func _copying(_ str: String) -> String {
    return String._copying(str[...])
  }
  @_alwaysEmitIntoClient
  @inline(never) // slow-path
  internal static func _copying(_ str: Substring) -> String {
    if _fastPath(str._wholeGuts.isFastUTF8) {
      return str._wholeGuts.withFastUTF8(range: str._offsetRange) {
        String._uncheckedFromUTF8($0)
      }
    }
    let count = str.uft8.count
    return String(unsafeUninitializedCapacity: count) { buffer in
      var (_, end) = str.utf8._copyContents(initializing: buffer)
      _internalInvariant(end == buffer.endIndex, "Buffer too large for new string")
      return count
    }
  }

  @usableFromInline
  @available(SwiftStdlib 6.0, *)
  internal static func _validate<Encoding: Unicode.Encoding>(
    _ input: UnsafeBufferPointer<Encoding.CodeUnit>,
    as encoding: Encoding.Type
  ) -> String? {
    if encoding.CodeUnit.self == UInt8.self {
      let bytes = _identityCast(input, to: UnsafeBufferPointer<UInt8>.self)
      if encoding.self == UTF8.self {
        guard case .success(let info) = validateUTF8(bytes) else { return nil }
        return String._uncheckedFromUTF8(bytes, asciiPreScanResult: info.isASCII)
      } else if encoding.self == Unicode.ASCII.self {
        guard _allASCII(bytes) else { return nil }
        return String._uncheckedFromASCII(bytes)
      }
    }

    // slow-path
    var isASCII = true
    var buffer: UnsafeMutableBufferPointer<UInt8>
    buffer = UnsafeMutableBufferPointer.allocate(capacity: input.count*3)
    var written = buffer.startIndex

    var parser = Encoding.ForwardParser()
    var input = input.makeIterator()

    transcodingLoop:
    while true {
      switch parser.parseScalar(from: &input) {
      case .valid(let s):
        let scalar = Encoding.decode(s)
        guard let utf8 = Unicode.UTF8.encode(scalar) else {
          // transcoding error: clean up and return nil
          fallthrough
        }
        if buffer.count < written + utf8.count {
          let newCapacity = buffer.count + (buffer.count >> 1)
          let copy: UnsafeMutableBufferPointer<UInt8>
          copy = UnsafeMutableBufferPointer.allocate(capacity: newCapacity)
          let copied = copy.moveInitialize(
            fromContentsOf: buffer.prefix(upTo: written)
          )
          buffer.deallocate()
          buffer = copy
          written = copied
        }
        if isASCII && utf8.count > 1 {
          isASCII = false
        }
        written = buffer.suffix(from: written).initialize(fromContentsOf: utf8)
        break
      case .error:
        // validation error: clean up and return nil
        buffer.prefix(upTo: written).deinitialize()
        buffer.deallocate()
        return nil
      case .emptyInput:
        break transcodingLoop
      }
    }

    let storage = buffer.baseAddress.map {
      __SharedStringStorage(
        _mortal: $0,
        countAndFlags: _StringObject.CountAndFlags(
          count: buffer.startIndex.distance(to: written),
          isASCII: isASCII,
          isNFC: isASCII,
          isNativelyStored: false,
          isTailAllocated: false
        )
      )
    }
    return storage?.asString
  }
}
