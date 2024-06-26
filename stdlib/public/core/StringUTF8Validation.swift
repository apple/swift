//===--- StringUTF8Validation.swift ---------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

private func _isUTF8MultiByteLeading(_ x: UInt8) -> Bool {
  return (0xC2...0xF4).contains(x)
}

private func _isNotOverlong_F0(_ x: UInt8) -> Bool {
  return (0x90...0xBF).contains(x)
}

private func _isNotOverlong_F4(_ x: UInt8) -> Bool {
  return UTF8.isContinuation(x) && x <= 0x8F
}

private func _isNotOverlong_E0(_ x: UInt8) -> Bool {
  return (0xA0...0xBF).contains(x)
}

private func _isNotOverlong_ED(_ x: UInt8) -> Bool {
  return UTF8.isContinuation(x) && x <= 0x9F
}

internal struct UTF8ExtraInfo: Equatable {
  public var isASCII: Bool
}

internal enum UTF8ValidationResult {
  case success(UTF8ExtraInfo)
  case error(toBeReplaced: Range<Int>)
}

extension UTF8ValidationResult: Equatable {}

private struct UTF8ValidationError: Error {}

internal func validateUTF8(_ buf: UnsafeBufferPointer<UInt8>) -> UTF8ValidationResult {
  if _allASCII(buf) {
    return .success(UTF8ExtraInfo(isASCII: true))
  }

  var iter = buf.makeIterator()
  var lastValidIndex = buf.startIndex

  @inline(__always) func guaranteeIn(_ f: (UInt8) -> Bool) throws(UTF8ValidationError) {
    guard let cu = iter.next() else { throw UTF8ValidationError() }
    guard f(cu) else { throw UTF8ValidationError() }
  }
  @inline(__always) func guaranteeContinuation() throws(UTF8ValidationError) {
    try guaranteeIn(UTF8.isContinuation)
  }

  func _legacyInvalidLengthCalculation(_ _buffer: (_storage: UInt32, ())) -> Int {
    // function body copied from UTF8.ForwardParser._invalidLength
    if _buffer._storage               & 0b0__1100_0000__1111_0000
                                     == 0b0__1000_0000__1110_0000 {
      // 2-byte prefix of 3-byte sequence. The top 5 bits of the decoded result
      // must be nonzero and not a surrogate
      let top5Bits = _buffer._storage & 0b0__0010_0000__0000_1111
      if top5Bits != 0 && top5Bits   != 0b0__0010_0000__0000_1101 { return 2 }
    }
    else if _buffer._storage                & 0b0__1100_0000__1111_1000
                                           == 0b0__1000_0000__1111_0000
    {
      // Prefix of 4-byte sequence. The top 5 bits of the decoded result
      // must be nonzero and no greater than 0b0__0100_0000
      let top5bits = UInt16(_buffer._storage & 0b0__0011_0000__0000_0111)
      if top5bits != 0 && top5bits.byteSwapped <= 0b0__0000_0100__0000_0000 {
        return _buffer._storage   & 0b0__1100_0000__0000_0000__0000_0000
                                 == 0b0__1000_0000__0000_0000__0000_0000 ? 3 : 2
      }
    }
    return 1
  }

  func _legacyNarrowIllegalRange(buf: Slice<UnsafeBufferPointer<UInt8>>) -> Range<Int> {
    var reversePacked: UInt32 = 0
    if let third = buf.dropFirst(2).first {
      reversePacked |= UInt32(third)
      reversePacked <<= 8
    }
    if let second = buf.dropFirst().first {
      reversePacked |= UInt32(second)
      reversePacked <<= 8
    }
    reversePacked |= UInt32(buf.first!)
    let _buffer: (_storage: UInt32, x: ()) = (reversePacked, ())
    let invalids = _legacyInvalidLengthCalculation(_buffer)
    return buf.startIndex ..< buf.startIndex + invalids
  }

  func findInvalidRange(_ buf: Slice<UnsafeBufferPointer<UInt8>>) -> Range<Int> {
    var endIndex = buf.startIndex
    var iter = buf.makeIterator()
    _ = iter.next()
    while let cu = iter.next(), UTF8.isContinuation(cu) {
      endIndex += 1
    }
    let illegalRange = Range(buf.startIndex...endIndex)
    _internalInvariant(illegalRange.clamped(to: (buf.startIndex..<buf.endIndex)) == illegalRange,
                 "illegal range out of full range")
    // FIXME: Remove the call to `_legacyNarrowIllegalRange` and return `illegalRange` directly
    return _legacyNarrowIllegalRange(buf: buf[illegalRange])
  }

  do {
    while let cu = iter.next() {
      if UTF8.isASCII(cu) { 
        lastValidIndex &+= 1
        continue 
      }
      if _slowPath(!_isUTF8MultiByteLeading(cu)) {
        func fail() throws(UTF8ValidationError) { throw UTF8ValidationError() }
        try fail()
      }
      switch cu {
      case 0xC2...0xDF:
        try guaranteeContinuation()
        lastValidIndex &+= 2
      case 0xE0:
        try guaranteeIn(_isNotOverlong_E0)
        try guaranteeContinuation()
        lastValidIndex &+= 3
      case 0xE1...0xEC:
        try guaranteeContinuation()
        try guaranteeContinuation()
        lastValidIndex &+= 3
      case 0xED:
        try guaranteeIn(_isNotOverlong_ED)
        try guaranteeContinuation()
        lastValidIndex &+= 3
      case 0xEE...0xEF:
        try guaranteeContinuation()
        try guaranteeContinuation()
        lastValidIndex &+= 3
      case 0xF0:
        try guaranteeIn(_isNotOverlong_F0)
        try guaranteeContinuation()
        try guaranteeContinuation()
        lastValidIndex &+= 4
      case 0xF1...0xF3:
        try guaranteeContinuation()
        try guaranteeContinuation()
        try guaranteeContinuation()
        lastValidIndex &+= 4
      case 0xF4:
        try guaranteeIn(_isNotOverlong_F4)
        try guaranteeContinuation()
        try guaranteeContinuation()
        lastValidIndex &+= 4
      default:
        Builtin.unreachable()
      }
    }
    return .success(UTF8ExtraInfo(isASCII: false))
  } catch {
    return .error(toBeReplaced: findInvalidRange(buf[lastValidIndex...]))
  }
}

internal func repairUTF8(_ input: UnsafeBufferPointer<UInt8>, firstKnownBrokenRange: Range<Int>) -> String {
  _internalInvariant(!input.isEmpty, "empty input doesn't need to be repaired")
  _internalInvariant(firstKnownBrokenRange.clamped(to: input.indices) == firstKnownBrokenRange)
  // During this process, `remainingInput` contains the remaining bytes to process. It's split into three
  // non-overlapping sub-regions:
  //
  //  1. `goodChunk` (may be empty) containing bytes that are known good UTF-8 and can be copied into the output String
  //  2. `brokenRange` (never empty) the next range of broken bytes,
  //  3. the remainder (implicit, will become the next `remainingInput`)
  //
  // At the beginning of the process, the `goodChunk` starts at the beginning and extends to just before the first
  // known broken byte. The known broken bytes are covered in the `brokenRange` and everything following that is
  // the remainder.
  // We then copy the `goodChunk` into the target buffer and append a UTF8 replacement character. `brokenRange` is
  // skipped (replaced by the replacement character) and we restart the same process. This time, `goodChunk` extends
  // from the byte after the previous `brokenRange` to the next `brokenRange`.
  var result = _StringGuts()
  let replacementCharacterCount = Unicode.Scalar._replacementCharacter.withUTF8CodeUnits { $0.count }
  result.reserveCapacity(input.count + 5 * replacementCharacterCount) // extra space for some replacement characters

  var brokenRange: Range<Int> = firstKnownBrokenRange
  var remainingInput = input
  repeat {
    _internalInvariant(!brokenRange.isEmpty, "broken range empty")
    _internalInvariant(!remainingInput.isEmpty, "empty remaining input doesn't need to be repaired")
    let goodChunk = remainingInput[..<brokenRange.startIndex]

    // very likely this capacity reservation does not actually do anything because we reserved space for the entire
    // input plus up to five replacement characters up front
    result.reserveCapacity(result.count + remainingInput.count + replacementCharacterCount)

    // we can now safely append the next known good bytes and a replacement character
    result.appendInPlace(UnsafeBufferPointer(rebasing: goodChunk),
                         isASCII: false /* appending replacement character anyway, so let's not bother */)
    Unicode.Scalar._replacementCharacter.withUTF8CodeUnits {
      result.appendInPlace($0, isASCII: false)
    }

    remainingInput = UnsafeBufferPointer(rebasing: remainingInput[brokenRange.endIndex...])
    switch validateUTF8(remainingInput) {
    case .success:
      result.appendInPlace(remainingInput, isASCII: false)
      return String(result)
    case .error(let newBrokenRange):
      brokenRange = newBrokenRange
    }
  } while !remainingInput.isEmpty
  return String(result)
}
