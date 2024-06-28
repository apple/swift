//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

extension Sequence where Element == Unicode.Scalar {
  internal var _internalNFC: Unicode._InternalNFC<Self> {
    Unicode._InternalNFC(self)
  }
}

extension Unicode {

  /// The contents of the source sequence, in Normalization Form C.
  ///
  /// Normalization to NFC preserves canonical equivalence.
  ///
  internal struct _InternalNFC<Source> where Source: Sequence<Unicode.Scalar> {

    internal let source: Source

    internal init(_ source: Source) {
      self.source = source
    }
  }
}

extension Unicode._InternalNFC: Sequence {

  internal consuming func makeIterator() -> Iterator {
    Iterator(source: source.makeIterator())
  }

  internal struct Iterator: IteratorProtocol {

    internal var source: Source.Iterator
    internal var normalizer: Unicode._NFCNormalizer

    internal init(source: Source.Iterator) {
      self.source = source
      if let strIter = source as? String.UnicodeScalarView.Iterator {
        self.normalizer = Unicode._NFCNormalizer(sourceString: strIter._guts)
      } else if let substrIter = source as? Substring.UnicodeScalarView.Iterator {
        self.normalizer = Unicode._NFCNormalizer(sourceString: substrIter._elements._wholeGuts)
      } else {
        self.normalizer = Unicode._NFCNormalizer()
      }
    }

    internal mutating func next() -> Unicode.Scalar? {
      normalizer.resume { source.next() } ?? normalizer.flush()
    }
  }
}

extension Unicode._InternalNFC: Sendable where Source: Sendable {}
extension Unicode._InternalNFC.Iterator: Sendable where Source.Iterator: Sendable {}

extension Unicode {

  /// A stateful normalizer, producing a single logical stream
  /// of normalized text from chunked inputs.
  ///
  /// To use the normalizer, first create an instance.
  /// Next, feed it a chunk of a text stream using the `resume(consuming:)`
  /// function. The normalizer will consume from the stream and buffer
  /// it as needed, so continue feeding the same source until
  /// it returns `nil`, indicating that the source was exhausted.
  ///
  /// ```swift
  /// var normalizer = Unicode.NFCNormalizer()
  ///
  /// var input: some IteratorProtocol<Unicode.Scalar> = ...
  /// while let scalar = normalizer.resume(consuming: &input) {
  ///   print(scalar)
  /// }
  ///
  /// // assert(input.next() == nil)
  /// ```
  ///
  /// You may continue consuming sources until you reach the end
  /// of the logical text stream. Once you reach the end,
  /// call `flush()` to drain any remaining content
  /// from the normalizer's buffers.
  ///
  /// ```swift
  /// while let scalar = normalizer.flush() {
  ///   print(scalar)
  /// }
  /// ```
  ///
  /// The chunks of input text do not need to be aligned on any normalization
  /// boundary. The normalizer state has value semantics, so it is possible
  /// to copy and store and is inherently thread-safe.
  ///
  internal struct _NFCNormalizer: Sendable {

    internal enum State {
      case emittingSegment
      case consuming
    }

    internal var state = State.consuming
    internal var isTerminated = false
    internal var sourceIsAlreadyNFC = false

    internal var nfd = Unicode._NFDNormalizer()
    internal var buffer = Unicode._NormDataBuffer()
    // This is our starter that is currently being composed with other scalars
    // into new scalars. For example, "e\u{301}", here our first scalar is 'e',
    // which is a starter, thus we assign composee to this 'e' and move to the
    // next scalar. We attempt to compose our composee, 'e', with '\u{301}' and
    // find that there is a composition. Thus our new composee is now 'é' and
    // we continue to try and compose following scalars with this composee.
    internal var composee = Optional<Unicode.Scalar>.none

    internal init(sourceString: borrowing _StringGuts) {
      sourceIsAlreadyNFC = sourceString.isNFC
    }

    /// Creates a new normalizer.
    ///
    internal init() { }

    /// Resume normalizing the text stream.
    ///
    /// Each call to `resume` returns the next scalar in the normalized output,
    /// consuming elements from the given source as necessary.
    ///
    /// If the normalizer returns `nil`, the source was exhausted.
    /// One a source is exhausted, you may:
    ///
    /// - Call `resume` again some time later with a different source
    ///   to continue processing the same logical text stream, or
    ///
    /// - Call `flush` in order to mark the end of the stream
    ///   and consume data remaining in the normalizer's internal buffers.
    ///
    /// Typical usage looks like the following:
    ///
    /// ```swift
    /// var normalizer = Unicode.NFCNormalizer()
    ///
    /// var input: some IteratorProtocol<Unicode.Scalar> = ...
    /// while let scalar = normalizer.resume(consuming: &input) {
    ///   print(scalar)
    /// }
    ///
    /// // We could resume again, consuming from another input here.
    /// // Finally, when we are done consuming inputs:
    ///
    /// while let scalar = normalizer.flush() {
    ///   print(scalar)
    /// }
    /// ```
    ///
    /// The normalizer consumes data from the source as needed,
    /// meaning even if a call to `resume` returns a value,
    /// that value may have come from the normalizer's internal buffers
    /// without consuming the input source at all.
    ///
    /// Be careful to ensure each input source has been fully consumed
    /// before moving on to the next source (marked by `resume` returning `nil`).
    ///
    internal mutating func resume(
      consuming source: inout some IteratorProtocol<Unicode.Scalar>
    ) -> Unicode.Scalar? {
      resume(consuming: { source.next() })
    }

    // Intended ABI barrier for resume(consuming: inout some IteratorProtocol<Unicode.Scalar>).
    // when it becomes public.
    internal mutating func resume(
      consuming nextFromSource: () -> Unicode.Scalar?
    ) -> Unicode.Scalar? {

      guard !isTerminated else {
        return nil
      }
      guard !sourceIsAlreadyNFC else {
        return nextFromSource()
      }
      return _resume(consumingNFD: { $0.nfd._resume(consuming: nextFromSource) })
    }

    /// Marks the end of the text stream and
    /// returns the next scalar from the normalizer's internal buffer.
    ///
    /// Once you have finished feeding input data to the normalizer,
    /// call `flush` until it returns `nil`.
    ///
    /// ```swift
    /// while let scalar = normalizer.flush() {
    ///   print(scalar)
    /// }
    /// ```
    ///
    /// After calling `flush`, all future calls to `resume`
    /// will immediately return `nil` without consuming from its source.
    /// This allows optional chaining to be used to
    /// fully normalize a stream:
    ///
    /// ```swift
    /// // Normalize the concatenation of inputA and inputB
    ///
    /// while let scalar =
    ///   normalizer.resume(consuming: &inputA) ??
    ///   normalizer.resume(consuming: &inputB) ??
    ///   normalizer.flush()
    /// {
    ///   print(scalar)
    /// }
    /// ```
    ///
    internal mutating func flush() -> Unicode.Scalar? {

      isTerminated = true

      guard !sourceIsAlreadyNFC else {
        return nil
      }

      // Process anything remaining from the NFD normalizer.
      if let next = _resume(consumingNFD: { $0.nfd._flush() }) {
        return next
      }

      // If we have a leftover composee, make sure to return it.
      // We may still have things in the buffer which are not complete segments.
      return composee.take() ?? buffer.next()?.scalar
    }
  }
}

extension Unicode._NFCNormalizer {

  @inline(never)
  internal mutating func _resume(
    consumingNFD nextNFD: (inout Self) -> ScalarAndNormData?
  ) -> Unicode.Scalar? {

    switch state {
    case .emittingSegment:

      if let buffered = buffer.next() {
        return buffered.scalar
      }
      state = .consuming
      fallthrough

    case .consuming:

      while let current = nextNFD(&self) {

        // The first starter in the sequence is our initial 'composee'.
        // Any scalars preceding the first starter have nothing to compose with
        // and are just emitted directly.

        guard let currentComposee = composee else {
          guard current.normData.canonicalCombiningClass == .notReordered else {
            return current.scalar
          }
          composee = current.scalar
          continue
        }

        guard let lastBufferedNormData = buffer.last?.normData else {

          // The buffer is empty so we have a simple Non-Blocked Pair,
          // <composee, current>. Look for an equivalent Primary Composite.
          // If 'current' is NFC_QC, we already know there won't be a composite.

          guard
            !current.normData.isNFCQC,
            let composed = compose(currentComposee, andNonNFCQC: current.scalar)
          else {

            // No Primary Composite found.
            // If 'current' is a starter, yield 'composee',
            // and begin a new segment with 'current' as the new 'composee'.
            // Otherwise, 'current' is a non-composing mark
            // that we need to buffer until we are finished composing.

            if current.normData.canonicalCombiningClass == .notReordered {
              composee = current.scalar
              return currentComposee
            }
            buffer.append(current)
            continue
          }

          // Primary Composite found. 
          // It becomes our new 'composee' and 'current' is discarded.

          composee = composed
          continue
        }

        // We have the sequence <composee, [...buffer contents...], current>.
        // Check whether 'current' may compose with 'composee',
        // or whether it is blocked by the buffer contents.
        //
        // Blocking refers to the presence of a scalar X in the buffer
        // where CCC(X) == 0 or CCC(X) >= CCC(current).
        //
        // Example:
        //
        // - "a\u{0305}\u{0300}b" (a̅̀b) => NFC "a\u{0305}\u{0300}b" (a̅̀b)
        // - "a\u{0300}\u{0305}b" (à̅b) => NFC "\u{00E0}\u{0305}b"  (à̅b)
        //         ^^^     ^^^
        //
        // These strings contain two combining marks with the same combining
        // class: U+0305 COMBINING OVERLINE and U+0300 COMBINING GRAVE ACCENT.
        // Because these marks have the same class, they cannot be reordered
        // (their existing order is important). In one ordering, the accent
        // appears above the overline, and in the other the order is reversed.
        //
        // It turns out, there is no composite for <"a", overline>,
        // but there is one for <"a", grave accent>: the
        // U+00E0 LATIN SMALL LETTER A WITH GRAVE we see in the second example.
        //
        // Despite the overline not composing, it would be wrong
        // if the grave accent could squeeze ahead of it
        // via composition with the "a".
        // So the presence of the overline must block the composition.

        _internalInvariant(
          lastBufferedNormData.canonicalCombiningClass != .notReordered,
          "We never buffer starters"
        )

        // Since we consume an NFD stream
        // the buffer contents are already in canonical order,
        // and 'lastBufferedNormData' has the highest CCC in the buffer.

        _internalInvariant(
          lastBufferedNormData.canonicalCombiningClass <= current.normData.canonicalCombiningClass
          || current.normData.canonicalCombiningClass == .notReordered,
          "NFD stream not in canonical order"
        )

        guard lastBufferedNormData.canonicalCombiningClass < current.normData.canonicalCombiningClass else {

          // 'current' is blocked from composing with 'composee'.
          //
          // If 'current' is a starter, yield 'composee', 
          // emit the segment that we have in the buffer,
          // and begin a new segment with 'current' as the new 'composee'.
          // Otherwise, 'current' is a non-composing mark
          // that we need to buffer until we are finished composing.

          if current.normData.canonicalCombiningClass == .notReordered {
            composee = current.scalar
            state = .emittingSegment
            return currentComposee
          }
          buffer.append(current)
          continue
        }

        _internalInvariant(current.normData.canonicalCombiningClass != .notReordered)

        // Look for a Primary Composite equivalent to <composee, current>.
        // If 'current' is NFC_QC, we already know there won't be any composite.

        guard
          !current.normData.isNFCQC,
          let composed = compose(currentComposee, andNonNFCQC: current.scalar)
        else {

          // No Primary Composite found.
          // We know 'current' is not a starter, so it is a non-composing mark
          // that we need to buffer until we are finished composing.
          buffer.append(current)
          continue
        }

        // Primary Composite found.
        // It becomes our new 'composee', and 'current' is discarded.

        composee = composed
      }

      // NFD source is exhausted.
      return nil
    }
  }

  private func compose(
    _ x: Unicode.Scalar,
    andNonNFCQC y: Unicode.Scalar
  ) -> Unicode.Scalar? {

    if let hangul = composeHangul(x, and: y) {
      return hangul
    }

    // Otherwise, lookup the composition.
    let composition = _swift_stdlib_getComposition(x.value, y.value)

    guard composition != .max else {
      return nil
    }

    return Unicode.Scalar(_value: composition)
  }

  @inline(never)
  private func composeHangul(
    _ x: Unicode.Scalar,
    and y: Unicode.Scalar
  ) -> Unicode.Scalar? {
    // L = Hangul leading consonants
    let L: (base: UInt32, count: UInt32) = (base: 0x1100, count: 19)
    // V = Hangul vowels
    let V: (base: UInt32, count: UInt32) = (base: 0x1161, count: 21)
    // T = Hangul tail consonants
    let T: (base: UInt32, count: UInt32) = (base: 0x11A7, count: 28)
    // N = Number of precomposed Hangul syllables that start with the same
    //     leading consonant. (There is no base for N).
    let N: (base: UInt32, count: UInt32) = (base: 0x0, count: 588)
    // S = Hangul precomposed syllables
    let S: (base: UInt32, count: UInt32) = (base: 0xAC00, count: 11172)

    switch (x.value, y.value) {
    // Check for Hangul (L, V) -> LV compositions.
    case (L.base ..< L.base &+ L.count, V.base ..< V.base &+ V.count):
      let lIdx = x.value &- L.base
      let vIdx = y.value &- V.base
      let lvIdx = lIdx &* N.count &+ vIdx &* T.count
      let s = S.base &+ lvIdx
      return Unicode.Scalar(_value: s)

    // Check for Hangul (LV, T) -> LVT compositions.
    case (S.base ..< S.base &+ S.count, T.base &+ 1 ..< T.base &+ T.count):
      if (x.value &- S.base) % T.count == 0 {
        return Unicode.Scalar(_value: x.value &+ y.value &- T.base)
      } else {
        fallthrough
      }

    default:
      return nil
    }
  }
}
