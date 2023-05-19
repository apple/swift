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
//
// Defines the Hasher struct, representing Swift's standard hash function.
//
//===----------------------------------------------------------------------===//

import SwiftShims

@inline(__always)
internal func _loadPartialUnalignedUInt64LE(
  _ p: UnsafeRawPointer,
  byteCount: Int
) -> UInt64 {
  var result: UInt64 = 0
  switch byteCount {
  case 7:
    result |= UInt64(p.load(fromByteOffset: 6, as: UInt8.self)) &<< 48
    fallthrough
  case 6:
    result |= UInt64(p.load(fromByteOffset: 5, as: UInt8.self)) &<< 40
    fallthrough
  case 5:
    result |= UInt64(p.load(fromByteOffset: 4, as: UInt8.self)) &<< 32
    fallthrough
  case 4:
    result |= UInt64(p.load(fromByteOffset: 3, as: UInt8.self)) &<< 24
    fallthrough
  case 3:
    result |= UInt64(p.load(fromByteOffset: 2, as: UInt8.self)) &<< 16
    fallthrough
  case 2:
    result |= UInt64(p.load(fromByteOffset: 1, as: UInt8.self)) &<< 8
    fallthrough
  case 1:
    result |= UInt64(p.load(fromByteOffset: 0, as: UInt8.self))
    fallthrough
  case 0:
    return result
  default:
    _internalInvariantFailure()
  }
}

extension Hasher {
  /// This is a buffer for segmenting arbitrary data into 8-byte chunks.  Buffer
  /// storage is represented by a single 64-bit value in the format used by the
  /// finalization step of SipHash. (The least significant 56 bits hold the
  /// trailing bytes, while the most significant 8 bits hold the count of bytes
  /// appended so far, modulo 256. The count of bytes currently stored in the
  /// buffer is in the lower three bits of the byte count.)
  // FIXME: Remove @usableFromInline and @frozen once Hasher is resilient.
  // rdar://problem/38549901
  @usableFromInline @frozen
  internal struct _TailBuffer {
    // msb                                                             lsb
    // +---------+-------+-------+-------+-------+-------+-------+-------+
    // |byteCount|                 tail (<= 56 bits)                     |
    // +---------+-------+-------+-------+-------+-------+-------+-------+
    internal var value: UInt64

    @inline(__always)
    internal init() {
      self.value = 0
    }

    @inline(__always)
    internal init(tail: UInt64, byteCount: UInt64) {
      // byteCount can be any value, but we only keep the lower 8 bits.  (The
      // lower three bits specify the count of bytes stored in this buffer.)
      // FIXME: This should be a single expression, but it causes exponential
      // behavior in the expression type checker <rdar://problem/42672946>.
      let shiftedByteCount: UInt64 = ((byteCount & 7) << 3)
      let mask: UInt64 = (1 << shiftedByteCount - 1)
      _internalInvariant(tail & ~mask == 0)
      self.value = (byteCount &<< 56 | tail)
    }

    @inline(__always)
    internal init(tail: UInt64, byteCount: Int) {
      self.init(tail: tail, byteCount: UInt64(truncatingIfNeeded: byteCount))
    }

    internal var tail: UInt64 {
      @inline(__always)
      get { return value & ~(0xFF &<< 56) }
    }

    internal var byteCount: UInt64 {
      @inline(__always)
      get { return value &>> 56 }
    }

    @inline(__always)
    internal mutating func append(_ bytes: UInt64) -> UInt64 {
      let c = byteCount & 7
      if c == 0 {
        value = value &+ (8 &<< 56)
        return bytes
      }
      let shift = c &<< 3
      let chunk = tail | (bytes &<< shift)
      value = (((value &>> 56) &+ 8) &<< 56) | (bytes &>> (64 - shift))
      return chunk
    }

    @inline(__always)
    internal
    mutating func append(_ bytes: UInt64, count: UInt64) -> UInt64? {
      _internalInvariant(count >= 0 && count < 8)
      _internalInvariant(bytes & ~((1 &<< (count &<< 3)) &- 1) == 0)
      let c = byteCount & 7
      let shift = c &<< 3
      if c + count < 8 {
        value = (value | (bytes &<< shift)) &+ (count &<< 56)
        return nil
      }
      let chunk = tail | (bytes &<< shift)
      value = ((value &>> 56) &+ count) &<< 56
      if c + count > 8 {
        value |= bytes &>> (64 - shift)
      }
      return chunk
    }
  }
}

extension Hasher {
  // FIXME: Remove @usableFromInline and @frozen once Hasher is resilient.
  // rdar://problem/38549901
  @usableFromInline @frozen
  internal struct _Core {
    private var _buffer: _TailBuffer
    private var _state: Hasher._State

    @inline(__always)
    internal init(state: Hasher._State) {
      self._buffer = _TailBuffer()
      self._state = state
    }

    @inline(__always)
    internal init() {
      self.init(state: _State())
    }

    @inline(__always)
    internal init(seed: Int) {
      self.init(state: _State(seed: seed))
    }

    @inline(__always)
    internal mutating func combine(_ value: UInt) {
#if _pointerBitWidth(_64)
      combine(UInt64(truncatingIfNeeded: value))
#elseif _pointerBitWidth(_32)
      combine(UInt32(truncatingIfNeeded: value))
#else
#error("Unknown platform")
#endif
    }

    @inline(__always)
    internal mutating func combine(_ value: UInt64) {
      _state.compress(_buffer.append(value))
    }

    @inline(__always)
    internal mutating func combine(_ value: UInt32) {
      let value = UInt64(truncatingIfNeeded: value)
      if let chunk = _buffer.append(value, count: 4) {
        _state.compress(chunk)
      }
    }

    @inline(__always)
    internal mutating func combine(_ value: UInt16) {
      let value = UInt64(truncatingIfNeeded: value)
      if let chunk = _buffer.append(value, count: 2) {
        _state.compress(chunk)
      }
    }

    @inline(__always)
    internal mutating func combine(_ value: UInt8) {
      let value = UInt64(truncatingIfNeeded: value)
      if let chunk = _buffer.append(value, count: 1) {
        _state.compress(chunk)
      }
    }

    @inline(__always)
    internal mutating func combine(bytes: UInt64, count: Int) {
      _internalInvariant(count >= 0 && count < 8)
      let count = UInt64(truncatingIfNeeded: count)
      if let chunk = _buffer.append(bytes, count: count) {
        _state.compress(chunk)
      }
    }

    @inline(__always)
    internal mutating func combine(bytes: UnsafeRawBufferPointer) {
      var remaining = bytes.count
      guard remaining > 0 else { return }
      var data = bytes.baseAddress!

      // Load first unaligned partial word of data
      do {
        let start = UInt(bitPattern: data)
        let end = _roundUp(start, toAlignment: MemoryLayout<UInt64>.alignment)
        let c = min(remaining, Int(end - start))
        if c > 0 {
          let chunk = _loadPartialUnalignedUInt64LE(data, byteCount: c)
          combine(bytes: chunk, count: c)
          data += c
          remaining -= c
        }
      }
      _internalInvariant(
        remaining == 0 ||
        Int(bitPattern: data) & (MemoryLayout<UInt64>.alignment - 1) == 0)

      // Load as many aligned words as there are in the input buffer
      while remaining >= MemoryLayout<UInt64>.size {
        combine(UInt64(littleEndian: data.load(as: UInt64.self)))
        data += MemoryLayout<UInt64>.size
        remaining -= MemoryLayout<UInt64>.size
      }

      // Load last partial word of data
      _internalInvariant(remaining >= 0 && remaining < 8)
      if remaining > 0 {
        let chunk = _loadPartialUnalignedUInt64LE(data, byteCount: remaining)
        combine(bytes: chunk, count: remaining)
      }
    }

    @inline(__always)
    internal mutating func finalize() -> UInt64 {
      return _state.finalize(tailAndByteCount: _buffer.value)
    }
  }
}

/// The universal hash function used by `Set` and `Dictionary`.
///
/// `Hasher` can be used to map an arbitrary sequence of bytes to an integer
/// hash value. You can feed data to the hasher using a series of calls to
/// mutating `combine` methods. When you've finished feeding the hasher, the
/// hash value can be retrieved by calling `finalize()`:
///
///     var hasher = Hasher()
///     hasher.combine(23)
///     hasher.combine("Hello")
///     let hashValue = hasher.finalize()
///
/// Within the execution of a Swift program, `Hasher` guarantees that finalizing
/// it will always produce the same hash value as long as it is fed the exact
/// same sequence of bytes. However, the underlying hash algorithm is designed
/// to exhibit avalanche effects: slight changes to the seed or the input byte
/// sequence will typically produce drastic changes in the generated hash value.
///
/// - Note: Do not save or otherwise reuse hash values across executions of your
///   program. `Hasher` is usually randomly seeded, which means it will return
///   different values on every new execution of your program. The hash
///   algorithm implemented by `Hasher` may itself change between any two
///   versions of the standard library.
@frozen // FIXME: Should be resilient (rdar://problem/38549901)
public struct Hasher {
  internal var _core: _Core

  /// Creates a new hasher.
  ///
  /// The hasher uses a per-execution seed value that is set during process
  /// startup, usually from a high-quality random source.
  @_effects(releasenone)
  public init() {
    self._core = _Core()
  }

  /// Initialize a new hasher using the specified seed value.
  /// The provided seed is mixed in with the global execution seed.
  @usableFromInline
  @_effects(releasenone)
  internal init(_seed: Int) {
    self._core = _Core(seed: _seed)
  }

  /// Initialize a new hasher using the specified seed value.
  @usableFromInline // @testable
  @_effects(releasenone)
  internal init(_rawSeed: (UInt64, UInt64)) {
    self._core = _Core(state: _State(rawSeed: _rawSeed))
  }

  /// Indicates whether we're running in an environment where hashing needs to
  /// be deterministic. If this is true, the hash seed is not random, and hash
  /// tables do not apply per-instance perturbation that is not repeatable.
  /// This is not recommended for production use, but it is useful in certain
  /// test environments where randomization may lead to unwanted nondeterminism
  /// of test results.
  @inlinable
  internal static var _isDeterministic: Bool {
    @inline(__always)
    get {
      return _swift_stdlib_Hashing_parameters.deterministic
    }
  }

  /// The 128-bit hash seed used to initialize the hasher state. Initialized
  /// once during process startup.
  @inlinable // @testable
  internal static var _executionSeed: (UInt64, UInt64) {
    @inline(__always)
    get {
      // The seed itself is defined in C++ code so that it is initialized during
      // static construction.  Almost every Swift program uses hash tables, so
      // initializing the seed during the startup seems to be the right
      // trade-off.
      return (
        _swift_stdlib_Hashing_parameters.seed0,
        _swift_stdlib_Hashing_parameters.seed1)
    }
  }

  /// Adds the given value to this hasher, mixing its essential parts into the
  /// hasher state.
  ///
  /// - Parameter value: A value to add to the hasher.
  @inlinable
  @inline(__always)
  public mutating func combine<H: Hashable>(_ value: H) {
    value.hash(into: &self)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(_ value: UInt) {
    _core.combine(value)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(_ value: UInt64) {
    _core.combine(value)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(_ value: UInt32) {
    _core.combine(value)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(_ value: UInt16) {
    _core.combine(value)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(_ value: UInt8) {
    _core.combine(value)
  }

  @_effects(releasenone)
  @usableFromInline
  internal mutating func _combine(bytes value: UInt64, count: Int) {
    _core.combine(bytes: value, count: count)
  }

  /// Adds the contents of the given buffer to this hasher, mixing it into the
  /// hasher state.
  ///
  /// - Parameter bytes: A raw memory buffer.
  @_effects(releasenone)
  public mutating func combine(bytes: UnsafeRawBufferPointer) {
    _core.combine(bytes: bytes)
  }

  /// Finalize the hasher state and return the hash value.
  /// Finalizing invalidates the hasher; additional bits cannot be combined
  /// into it, and it cannot be finalized again.
  @_effects(releasenone)
  @usableFromInline
  internal mutating func _finalize() -> Int {
    return Int(truncatingIfNeeded: _core.finalize())
  }

  /// Finalizes the hasher state and returns the hash value.
  ///
  /// Finalizing consumes the hasher: it is illegal to finalize a hasher you
  /// don't own, or to perform operations on a finalized hasher. (These may
  /// become compile-time errors in the future.)
  ///
  /// Hash values are not guaranteed to be equal across different executions of
  /// your program. Do not save hash values to use during a future execution.
  ///
  /// - Returns: The hash value calculated by the hasher.
  @_effects(releasenone)
  public __consuming func finalize() -> Int {
    var core = _core
    return Int(truncatingIfNeeded: core.finalize())
  }

  @_effects(readnone)
  @usableFromInline
  internal static func _hash(seed: Int, _ value: UInt64) -> Int {
    var state = _State(seed: seed)
    state.compress(value)
    let tbc = _TailBuffer(tail: 0, byteCount: 8)
    return Int(truncatingIfNeeded: state.finalize(tailAndByteCount: tbc.value))
  }

  @_effects(readnone)
  @usableFromInline
  internal static func _hash(seed: Int, _ value: UInt) -> Int {
    var state = _State(seed: seed)
#if _pointerBitWidth(_64)
    _internalInvariant(UInt.bitWidth == UInt64.bitWidth)
    state.compress(UInt64(truncatingIfNeeded: value))
    let tbc = _TailBuffer(tail: 0, byteCount: 8)
#elseif _pointerBitWidth(_32)
    _internalInvariant(UInt.bitWidth < UInt64.bitWidth)
    let tbc = _TailBuffer(
      tail: UInt64(truncatingIfNeeded: value),
      byteCount: UInt.bitWidth &>> 3)
#else
#error("Unknown platform")
#endif
    return Int(truncatingIfNeeded: state.finalize(tailAndByteCount: tbc.value))
  }

  @_effects(readnone)
  @usableFromInline
  internal static func _hash(
    seed: Int,
    bytes value: UInt64,
    count: Int) -> Int {
    _internalInvariant(count >= 0 && count < 8)
    var state = _State(seed: seed)
    let tbc = _TailBuffer(tail: value, byteCount: count)
    return Int(truncatingIfNeeded: state.finalize(tailAndByteCount: tbc.value))
  }

  @_effects(readnone)
  @usableFromInline
  internal static func _hash(
    seed: Int,
    bytes: UnsafeRawBufferPointer) -> Int {
    var core = _Core(seed: seed)
    core.combine(bytes: bytes)
    return Int(truncatingIfNeeded: core.finalize())
  }
}
