//===--- StringNormalization.swift ----------------------------------------===//
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

internal enum _Normalization {
  // ICU's NFC unorm2 instance
  //
  // TODO(String performance): Should we cache one on TLS? Is this an expensive
  // call?
  internal static var _nfcNormalizer: OpaquePointer = {
    var err = __swift_stdlib_U_ZERO_ERROR
    let normalizer = __swift_stdlib_unorm2_getNFCInstance(&err)
    guard err.isSuccess else {
      // This shouldn't be possible unless some deep (unrecoverable) system
      // invariants are violated
      fatalError("Unable to talk to ICU")
    }
    return normalizer
  }()

  // When normalized in NFC, some segments may expand in size (e.g. some non-BMP
  // musical notes). This expansion is capped by the maximum expansion factor of
  // the normal form. For NFC, that is 3x.
  internal static let _maxNFCExpansionFactor = 3
  internal static let _maxUTF16toUTF8ExpansionFactor = 3

  internal typealias _SegmentOutputBuffer = _FixedArray16<UInt16>
}

//
// Pointer casting helpers
//
@inline(__always)
private func _unsafeMutableBufferPointerCast<T, U>(
  _ ptr: UnsafeMutablePointer<T>,
  _ count: Int,
  to: U.Type = U.self
) -> UnsafeMutableBufferPointer<U> {
  return UnsafeMutableBufferPointer(
    start: UnsafeMutableRawPointer(ptr).assumingMemoryBound(to: U.self),
    count: count
  )
}
@inline(__always)
private func _unsafeBufferPointerCast<T, U>(
  _ ptr: UnsafePointer<T>,
  _ count: Int,
  to: U.Type = U.self
) -> UnsafeBufferPointer<U> {
  return UnsafeBufferPointer(
    start: UnsafeRawPointer(ptr).assumingMemoryBound(to: U.self),
    count: count
  )
}
internal func _castOutputBuffer(
  _ ptr: UnsafeMutablePointer<_FixedArray16<UInt8>>,
  endingAt endIdx: Int = 16
) -> UnsafeMutableBufferPointer<UInt8> {
  let bufPtr: UnsafeMutableBufferPointer<UInt8> =
    _unsafeMutableBufferPointerCast(
      ptr, 16)
  return UnsafeMutableBufferPointer<UInt8>(rebasing: bufPtr[..<endIdx])
}
internal func _castOutputBuffer(
  _ ptr: UnsafeMutablePointer<_Normalization._SegmentOutputBuffer>,
  endingAt endIdx: Int = _Normalization._SegmentOutputBuffer.capacity
) -> UnsafeMutableBufferPointer<UInt16> {
  let bufPtr: UnsafeMutableBufferPointer<UInt16> =
    _unsafeMutableBufferPointerCast(
      ptr, _Normalization._SegmentOutputBuffer.capacity)
  return UnsafeMutableBufferPointer<UInt16>(rebasing: bufPtr[..<endIdx])
}
internal func _castOutputBuffer(
  _ ptr: UnsafePointer<_Normalization._SegmentOutputBuffer>,
  endingAt endIdx: Int = _Normalization._SegmentOutputBuffer.capacity
) -> UnsafeBufferPointer<UInt16> {
  let bufPtr: UnsafeBufferPointer<UInt16> =
    _unsafeBufferPointerCast(
      ptr, _Normalization._SegmentOutputBuffer.capacity)
  return UnsafeBufferPointer<UInt16>(rebasing: bufPtr[..<endIdx])
}

extension _StringGuts {
  internal func foreignHasNormalizationBoundary(
    before index: String.Index
  ) -> Bool {
    let offset = index._encodedOffset
    if offset == 0 || offset == count {
      return true
    }

    let scalar = foreignErrorCorrectedScalar(startingAt: index).0
    return scalar._hasNormalizationBoundaryBefore
  }
}
extension UnsafeBufferPointer where Element == UInt8 {
  internal func hasNormalizationBoundary(before index: Int) -> Bool {
    if index == 0 || index == count {
      return true
    }
    _internalInvariant(!UTF8.isContinuation(self[_unchecked: index]))

    // Sub-300 latiny fast-path
    if self[_unchecked: index] < 0xCC { return true }

    let cu = _decodeScalar(self, startingAt: index).0
    return cu._hasNormalizationBoundaryBefore
  }
}

extension Unicode.Scalar {
  // Normalization boundary - a place in a string where everything left of the
  // boundary can be normalized independently from everything right of the
  // boundary. The concatenation of each result is the same as if the entire
  // string had been normalized as a whole.
  //
  // Normalization segment - a sequence of code units between two normalization
  // boundaries (without any boundaries in the middle). Note that normalization
  // segments can, as a process of normalization, expand, contract, and even
  // produce new sub-segments.

  // Whether this scalar value always has a normalization boundary before it.
  @inline(__always) // common fast-path
  internal var _hasNormalizationBoundaryBefore: Bool {
    // Fast-path: All scalars up through U+02FF are NFC and have boundaries
    // before them
    if self.value < 0x300 { return true }

    _internalInvariant(Int32(exactly: self.value) != nil, "top bit shouldn't be set")
    let value = Int32(bitPattern: self.value)
    return 0 != __swift_stdlib_unorm2_hasBoundaryBefore(
      _Normalization._nfcNormalizer, value)
  }
  @inline(__always) // common fast-path
  internal var _isNFCQCYes: Bool {
    // Fast-path: All scalars up through U+02FF are NFC and have boundaries
    // before them
    if self.value < 0x300 { return true }

    return __swift_stdlib_u_getIntPropertyValue(
      Builtin.reinterpretCast(value), __swift_stdlib_UCHAR_NFC_QUICK_CHECK
    ) == 1
  }

  // Quick check if a scalar is NFC and a segment starter
  internal var _isNFCStarter: Bool {
    // Otherwise, consult the properties
    return self._hasNormalizationBoundaryBefore && self._isNFCQCYes
  }
}

extension UnsafeBufferPointer where Element == UInt8 {
  internal func isOnUnicodeScalarBoundary(_ index: Int) -> Bool {
    guard index < count else {
      _internalInvariant(index == count)
      return true
    }
    return !UTF8.isContinuation(self[index])
  }
  
}

//If this returns nil, it means the outputBuffer ran out of space
internal func _tryNormalize(
  _ input: UnsafeBufferPointer<UInt16>,
  into outputBuffer:
    UnsafeMutablePointer<_Normalization._SegmentOutputBuffer>
) -> Int? {
  return _tryNormalize(input, into: _castOutputBuffer(outputBuffer))
}

//If this returns nil, it means the outputBuffer ran out of space
internal func _tryNormalize(
  _ input: UnsafeBufferPointer<UInt16>,
  into outputBuffer: UnsafeMutableBufferPointer<UInt16>
) -> Int? {
  var err = __swift_stdlib_U_ZERO_ERROR
  let count = __swift_stdlib_unorm2_normalize(
    _Normalization._nfcNormalizer,
    input.baseAddress._unsafelyUnwrappedUnchecked,
    numericCast(input.count),
    outputBuffer.baseAddress._unsafelyUnwrappedUnchecked,
    numericCast(outputBuffer.count),
    &err
  )
  guard err.isSuccess else {
    // The output buffer needs to grow
    return nil
  }
  return numericCast(count)
}

internal struct NormalizationResult {
  var amountFilled: Int
  var nextReadPosition: String.Index
  var allocatedBuffers: Bool
}

//If this returns nil, it means the outputBuffer ran out of space
@_effects(releasenone)
private func fastFill(
  _ sourceBuffer: UnsafeBufferPointer<UInt8>,
  _ outputBuffer: UnsafeMutableBufferPointer<UInt8>
) -> (read: Int, written: Int)? {
  let outputBufferThreshold = outputBuffer.count - 4
  
  // TODO: Additional fast-path: All CCC-ascending NFC_QC segments are NFC
  // TODO: Just freakin do normalization and don't bother with ICU
  var outputCount = 0
  var inputCount = 0
  let inputEnd = sourceBuffer.count
  while inputCount < inputEnd && outputCount < outputBufferThreshold {
    // TODO: Slightly faster code-unit scan for latiny (<0xCC)

    // Check scalar-based fast-paths
    let (scalar, len) = _decodeScalar(sourceBuffer, startingAt: inputCount)
    _internalInvariant(inputCount &+ len <= inputEnd)

    if _slowPath(
         !sourceBuffer.hasNormalizationBoundary(before: inputCount &+ len)
      || !scalar._isNFCStarter
    ) {
      break
    }
    inputCount &+= len

    for cu in UTF8.encode(scalar)._unsafelyUnwrappedUnchecked {
      outputBuffer[_unchecked: outputCount] = cu
      outputCount &+= 1
    }

    _internalInvariant(inputCount == outputCount,
      "non-normalizing UTF-8 fast path should be 1-to-1 in code units")
  }
  return outputCount > 0 ? (inputCount, outputCount) : nil
}

//Transcodes a single segment from the scalars provided by the closure to the outputBuffer as UTF16
//If this returns nil, it means the outputBuffer ran out of space
private func copyUTF16Segment(
  boundedBy range: Range<Int>,
  into outputBuffer: UnsafeMutableBufferPointer<UInt16>,
  _ f: (Int) -> (Unicode.Scalar, Int)
) -> (read: Int, written: Int)? {
  var readIndex = range.lowerBound
  var outputWriteIndex = 0
  let outputCount = outputBuffer.count
  while readIndex != range.upperBound {
    let (scalar, length) = f(readIndex)
    if scalar._hasNormalizationBoundaryBefore && readIndex != range.lowerBound {
      break
    }
    
    readIndex += length
    
    for cu in scalar.utf16 {
      if outputWriteIndex < outputCount {
        outputBuffer[outputWriteIndex] = cu
        outputWriteIndex += 1
      } else {
        return nil
      }
    }
  }
  return (readIndex - range.lowerBound, outputWriteIndex)
}

//transcodes the UTF16 segment stored in soureceBuffer into the outputBuffer as UTF8
//If this returns nil, it means the outputBuffer ran out of space
private func transcodeValidUTF16ToUTF8(
  _ sourceBuffer: UnsafeBufferPointer<UInt16>,
  into outputBuffer: UnsafeMutableBufferPointer<UInt8>
) -> Int? {
  var readIndex = 0
  var writeIndex = 0
  let outputCount = outputBuffer.count
  let sourceCount = sourceBuffer.count
  
  while readIndex < sourceCount {
    let (scalar, length) = _decodeScalar(sourceBuffer, startingAt: readIndex)
    //we don't need to check for normalization boundaries here because we are only transcoding
    //a single segment at this point
    
    readIndex += length
    
    for cu in UTF8.encode(scalar)._unsafelyUnwrappedUnchecked {
      if writeIndex < outputCount {
        outputBuffer[writeIndex] = cu
        writeIndex &+= 1
      } else {
        return nil
      }
    }
  }
  return writeIndex
}

internal enum _BufferToCopy {
  case none, output, icuInput, icuOutput
}

internal func _allocateBuffers(
  sourceCount count: Int,
  preserveDataIn bufferToCopy: _BufferToCopy,
  outputBuffer: inout UnsafeMutableBufferPointer<UInt8>,
  icuInputBuffer: inout UnsafeMutableBufferPointer<UInt16>,
  icuOutputBuffer: inout UnsafeMutableBufferPointer<UInt16>
) {
  let output = count * _Normalization._maxNFCExpansionFactor * _Normalization._maxUTF16toUTF8ExpansionFactor
  let icuOutput = count * _Normalization._maxNFCExpansionFactor
  let newOutputBuffer = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: output)
  let newICUInputBuffer = UnsafeMutableBufferPointer<UInt16>.allocate(capacity: count)
  let newICUOutputBuffer = UnsafeMutableBufferPointer<UInt16>.allocate(capacity: icuOutput)
  
  switch bufferToCopy {
  case .none:
    break
  case .output:
    let (_, written) = newOutputBuffer.initialize(from: outputBuffer)
    _internalInvariant(written == 16)
  case .icuInput:
    let (_, written) = newICUInputBuffer.initialize(from: icuInputBuffer)
    _internalInvariant(written == 16)
  case .icuOutput:
    let (_, written) = newICUOutputBuffer.initialize(from: icuOutputBuffer)
    _internalInvariant(written == 16)
  }
  
  outputBuffer = newOutputBuffer
  icuInputBuffer = newICUInputBuffer
  icuOutputBuffer = newICUOutputBuffer
}

internal func _fastNormalize(
  readIndex: String.Index,
  sourceBuffer: UnsafeBufferPointer<UInt8>,
  outputBuffer: inout UnsafeMutableBufferPointer<UInt8>,
  icuInputBuffer: inout UnsafeMutableBufferPointer<UInt16>,
  icuOutputBuffer: inout UnsafeMutableBufferPointer<UInt16>
) -> NormalizationResult {
  let start = readIndex._encodedOffset
  let rebasedSourceBuffer = UnsafeBufferPointer(rebasing: sourceBuffer[start...])
  if let (read, filled) = fastFill(rebasedSourceBuffer, outputBuffer) {
    let nextIndex = readIndex.encoded(offsetBy: read)
    _internalInvariant(sourceBuffer.isOnUnicodeScalarBoundary(nextIndex._encodedOffset))
    
    return NormalizationResult(
      amountFilled: filled, nextReadPosition: nextIndex, allocatedBuffers: false)
  }
  var allocatedBuffers = false
  func performWithAllocationIfNecessary<R>(
    preserving preserveDataIn: _BufferToCopy, _ f: () -> R?
  ) -> R {
    if let result = f() {
      return result
    }
    _allocateBuffers(
      sourceCount: sourceBuffer.count,
      preserveDataIn: preserveDataIn,
      outputBuffer: &outputBuffer,
      icuInputBuffer: &icuInputBuffer,
      icuOutputBuffer: &icuOutputBuffer)
    _internalInvariant(!allocatedBuffers)
    allocatedBuffers = true
    return f()!
  }
  
  let (read, filled) = performWithAllocationIfNecessary(preserving: .none) { () -> (Int, Int)? in
    return copyUTF16Segment(boundedBy: 0..<rebasedSourceBuffer.count, into: icuInputBuffer) {
      return _decodeScalar(rebasedSourceBuffer, startingAt: $0)
    }
  }
  
  let nextIndex = readIndex.encoded(offsetBy: read)
  _internalInvariant(sourceBuffer.isOnUnicodeScalarBoundary(nextIndex._encodedOffset))
  
  let normalized = performWithAllocationIfNecessary(preserving: .icuInput) { () -> Int? in
    return _tryNormalize(
      UnsafeBufferPointer(rebasing: icuInputBuffer[..<filled]), into: icuOutputBuffer)
  }
  
  let transcoded = performWithAllocationIfNecessary(preserving: .icuOutput) { () -> Int? in
    return transcodeValidUTF16ToUTF8(
      UnsafeBufferPointer<UInt16>(rebasing: icuOutputBuffer[..<normalized]),
      into: outputBuffer)
  }
  return NormalizationResult(
    amountFilled: transcoded, nextReadPosition: nextIndex, allocatedBuffers: allocatedBuffers)
}

internal func _foreignNormalize(
  readIndex: String.Index,
  endIndex: String.Index,
  guts: _StringGuts,
  outputBuffer: inout UnsafeMutableBufferPointer<UInt8>,
  icuInputBuffer: inout UnsafeMutableBufferPointer<UInt16>,
  icuOutputBuffer: inout UnsafeMutableBufferPointer<UInt16>
) -> NormalizationResult {
  var allocatedBuffers = false
  func performWithAllocationIfNecessary<R>(
    preserving preserveDataIn: _BufferToCopy, _ f: () -> R?
  ) -> R {
    if let result = f() {
      return result
    }
    _allocateBuffers(
      sourceCount: guts.count,
      preserveDataIn: preserveDataIn,
      outputBuffer: &outputBuffer,
      icuInputBuffer: &icuInputBuffer,
      icuOutputBuffer: &icuOutputBuffer)
    _internalInvariant(!allocatedBuffers)
    allocatedBuffers = true
    return f()!
  }
  let (read, filled) = performWithAllocationIfNecessary(preserving: .none) { () -> (Int, Int)? in
    let start = readIndex._encodedOffset
    let end = endIndex._encodedOffset
    return copyUTF16Segment(boundedBy: start..<end, into: icuInputBuffer) { gutsOffset in
      return guts.errorCorrectedScalar(startingAt: gutsOffset)
    }
  }
  
  let nextIndex = readIndex.encoded(offsetBy: read)
  _internalInvariant(guts.isOnUnicodeScalarBoundary(nextIndex))
  
  let normalized = performWithAllocationIfNecessary(preserving: .icuInput) { () -> Int? in
    return _tryNormalize(
      UnsafeBufferPointer(rebasing: icuInputBuffer[..<filled]), into: icuOutputBuffer)
  }
  
  let transcoded = performWithAllocationIfNecessary(preserving: .icuOutput) { () -> Int? in
    return transcodeValidUTF16ToUTF8(
      UnsafeBufferPointer<UInt16>(rebasing: icuOutputBuffer[..<normalized]),
      into: outputBuffer)
  }
  return NormalizationResult(
    amountFilled: transcoded, nextReadPosition: nextIndex, allocatedBuffers: allocatedBuffers)
}
