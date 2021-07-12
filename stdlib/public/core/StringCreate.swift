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
// String Creation Helpers
//===----------------------------------------------------------------------===//

internal func _allASCII(_ input: UnsafeBufferPointer<UInt8>) -> Bool {
  if input.isEmpty { return true }

  // NOTE: Avoiding for-in syntax to avoid bounds checks
  //
  // TODO(String performance): please remove this SIMD-ization when Swift compiler will be smart enough
  // to vectorize this simple loop on its own:
  // for i in 0..<count where ptr[i] & byteASCIIMask != 0 {
  //   return false
  // }

  let ptr = input.baseAddress._unsafelyUnwrappedUnchecked
  var i = 0

  let count = input.count
  let stride = MemoryLayout<UInt>.stride
  let simd4UintStride = MemoryLayout<SIMD4<UInt>>.stride
  assert(simd4UintStride == stride * 4) // Memory layout of SIMD4<UInt> should match one of 4 UInt words
  let address = Int(bitPattern: ptr)

  let wordASCIIMask = UInt(truncatingIfNeeded: 0x8080_8080_8080_8080 as UInt64)
  let byteASCIIMask = UInt8(truncatingIfNeeded: 0x80 as UInt8)
  let simd4ASCIIMask = SIMD4<UInt>(repeating: wordASCIIMask)
  let simd4Zero = SIMD4<UInt>(repeating: 0)

  // Bytes up to beginning of a word
  while (address &+ i) % stride != 0 && i < count {
    guard ptr[i] & byteASCIIMask == 0 else { return false }
    i &+= 1
  }

  // Words up to beginning of a 4-word
  while (address &+ i) % simd4UintStride != 0 && (i &+ stride) <= count {
    let word: UInt = UnsafePointer(
      bitPattern: address &+ i
    )._unsafelyUnwrappedUnchecked.pointee
    guard word & wordASCIIMask == 0 else { return false }
    i &+= stride
  }

  // Full 4-words
  while (i &+ simd4UintStride) <= count {
    let simd4: SIMD4<UInt> = UnsafePointer<SIMD4<UInt>>(
      bitPattern: address &+ i
    )._unsafelyUnwrappedUnchecked.pointee
    guard simd4 & simd4ASCIIMask == simd4Zero else { return false }
    i &+= simd4UintStride
  }

  // Full words
  while (i &+ stride) <= count {
    let word: UInt = UnsafePointer(
      bitPattern: address &+ i
    )._unsafelyUnwrappedUnchecked.pointee
    guard word & wordASCIIMask == 0 else { return false }
    i &+= stride
  }

  // Rest bytes up to end
  while i < count {
    guard ptr[i] & byteASCIIMask == 0 else { return false }
    i &+= 1
  }
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
    return Array(str.utf8).withUnsafeBufferPointer {
      String._uncheckedFromUTF8($0)
    }
  }
}
