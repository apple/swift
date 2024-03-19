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

internal func validateUTF8(_ buf: UnsafeBufferPointer<UInt8>) -> UTF8ValidationResult {

  @inline(__always) func checkNext(_ iter: inout UnsafeBufferPointer<UInt8>.Iterator, _ f: (UInt8) -> Bool) -> Bool {
    guard let cu = iter.next(), f(cu) else { return false }
    return true
  }
  @inline(__always) func checkNextIsContinuation(_ iter: inout UnsafeBufferPointer<UInt8>.Iterator) -> Bool {
    checkNext(&iter, UTF8.isContinuation)
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

  func findInvalidRange(from startOfInvalid: Int) -> Range<Int> {
    _internalInvariant(startOfInvalid < buf.endIndex, "startOfInvalid must be a valid index")
    var endOfInvalid = startOfInvalid &+ 1
    while endOfInvalid < buf.endIndex, UTF8.isContinuation(buf[endOfInvalid]) {
      endOfInvalid &+= 1
    }
    endOfInvalid &+= 1
    let illegalRange = Range(uncheckedBounds: (startOfInvalid, endOfInvalid))
    // FIXME: Remove the call to `_legacyNarrowIllegalRange` and return `illegalRange` directly
    return _legacyNarrowIllegalRange(buf: buf[illegalRange])
  }

  if _allASCII(buf) {
    return .success(UTF8ExtraInfo(isASCII: true))
  }

  var firstUnvalidated = buf.startIndex
  var iter = buf.makeIterator()
  while let cu = iter.next() {
    if UTF8.isASCII(cu) {
      firstUnvalidated &+= 1
      continue
    }
    if _slowPath(!_isUTF8MultiByteLeading(cu)) {
      return .error(toBeReplaced: findInvalidRange(from: firstUnvalidated))
    }
    let isValid: Bool
    switch cu {
    case 0xC2...0xDF:
      isValid = checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 2 }
    case 0xE0:
      isValid = checkNext(&iter, _isNotOverlong_E0)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 3 }
    case 0xE1...0xEC:
      isValid = checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 3 }
    case 0xED:
      isValid = checkNext(&iter, _isNotOverlong_ED)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 3 }
    case 0xEE...0xEF:
      isValid = checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 3 }
    case 0xF0:
      isValid = checkNext(&iter, _isNotOverlong_F0)
             && checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 4 }
    case 0xF1...0xF3:
      isValid = checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 4 }
    case 0xF4:
      isValid = checkNext(&iter, _isNotOverlong_F4)
             && checkNextIsContinuation(&iter)
             && checkNextIsContinuation(&iter)
      if isValid { firstUnvalidated &+= 4 }
    default:
      Builtin.unreachable()
    }
    if !isValid {
      return .error(toBeReplaced: findInvalidRange(from: firstUnvalidated))
    }
  }
  return .success(UTF8ExtraInfo(isASCII: false))
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
