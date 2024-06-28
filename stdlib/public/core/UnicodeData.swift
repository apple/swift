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

internal typealias ScalarAndNormData = (
  scalar: Unicode.Scalar,
  normData: Unicode._NormData
)

extension Unicode {
  // A wrapper type over the normalization data value we receive when we
  // lookup a scalar's normalization information. The layout of the underlying
  // 16 bit value we receive is as follows:
  //
  // 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
  // └───┬───┘ └──── CCC ────┘ └─┘ │
  //     │                      │  └── NFD_QC
  //     │                      └── NFC_QC
  //     └── Unused
  //
  // NFD_QC: This is a simple Yes/No on whether the scalar has canonical
  //         decomposition. Note: Yes is indicated via 0 instead of 1.
  //
  // NFC_QC: This is either Yes/No/Maybe on whether the scalar is NFC quick
  //         check. Yes, represented as 0, means the scalar can NEVER compose
  //         with another scalar previous to it. No, represented as 1, means the
  //         scalar can NEVER appear within a well formed NFC string. Maybe,
  //         represented as 2, means the scalar could appear with an NFC string,
  //         but further information is required to determine if that is the
  //         case. At the moment, we really only care about Yes/No.
  //
  // CCC: This is the canonical combining class property of a scalar that is
  //      used when sorting scalars of a normalization segment after NFD
  //      computation. A scalar with a CCC value of 128 can NEVER appear before
  //      a scalar with a CCC value of 100, unless there are normalization
  //      boundaries between them.
  //
  internal struct _NormData {
    var rawValue: UInt16

    var ccc: UInt8 {
      UInt8(truncatingIfNeeded: rawValue >> 3)
    }

    var canonicalCombiningClass: Unicode.CanonicalCombiningClass {
      Unicode.CanonicalCombiningClass(rawValue: ccc)
    }

    var isNFCQC: Bool {
      rawValue & 0x6 == 0
    }

    var isNFDQC: Bool {
      rawValue & 0x1 == 0
    }

    init(_ scalar: Unicode.Scalar, fastUpperbound: UInt32 = 0xC0) {
      if _fastPath(scalar.value < fastUpperbound) {
        // CCC = 0, NFC_QC = Yes, NFD_QC = Yes
        rawValue = 0
      } else {
        rawValue = _swift_stdlib_getNormData(scalar.value)

        // Because we don't store precomposed hangul in our NFD_QC data, these
        // will return true for NFD_QC when in fact they are not.
        if (0xAC00 ... 0xD7A3).contains(scalar.value) {
          // NFD_QC = false
          rawValue |= 0x1
        }
      }
    }

    init(rawValue: UInt16) {
      self.rawValue = rawValue
    }
  }
}

extension Unicode {
  // A wrapper type for normalization buffers in the NFC and NFD iterators.
  // This helps remove some of the buffer logic like removal and sorting out of
  // the iterators and into this type.
  internal struct _NormDataBuffer {
    var storage: [ScalarAndNormData] = []

    // This is simply a marker denoting that we've built up our storage, and
    // now everything within it needs to be emitted. We reverse the buffer and
    // pop elements from the back as a way to remove them.
    var isReversed = false

    var isEmpty: Bool {
      storage.isEmpty
    }

    var last: ScalarAndNormData? {
      storage.last
    }

    mutating func append(_ scalarAndNormData: ScalarAndNormData) {
      _internalInvariant(!isReversed)
      storage.append(scalarAndNormData)
    }

    // Removes the first element from the buffer. Note: it is not safe to append
    // to the buffer after this function has been called. We reverse the storage
    // internally for everything to be emitted out, so appending would insert
    // into the storage at the wrong location. One must continue to call this
    // function until a 'nil' return value has been received before appending.
    mutating func next() -> ScalarAndNormData? {
      guard !storage.isEmpty else {
        isReversed = false
        return nil
      }

      // If our storage hasn't been reversed yet, do so now.
      if !isReversed {
        storage.reverse()
        isReversed = true
      }

      return storage.removeLast()
    }

    // Sort the entire buffer based on the canonical combining class.
    mutating func sort() {
      storage._insertionSort(within: storage.indices) {
        $0.normData.ccc < $1.normData.ccc
      }
    }
  }
}

extension Unicode {
  // A wrapper type over the decomposition entry value we receive when we
  // lookup a scalar's canonical decomposition. The layout of the underlying
  // 32 bit value we receive is as follows:
  //
  //          Top 14 bits                   Bottom 18 bits
  //
  // 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
  // └───────── Index ─────────┘ └───────── Hashed Scalar ─────────┘
  //
  // Index: This is the direct index into '_swift_stdlib_nfd_decompositions'
  //        that points to a size byte indicating the overall size of the
  //        UTF-8 decomposition string. Following the size byte is said string.
  //
  // Hashed Scalar: Because perfect hashing doesn't know the original set of
  //                keys it was hashed with, we store the original scalar in the
  //                decomposition entry so that we can guard against scalars
  //                who happen to hash to the same index.
  //
  internal struct _DecompositionEntry {
    let rawValue: UInt32

    // Our original scalar is stored in the first 18 bits of this entry.
    var hashedScalar: Unicode.Scalar {
      Unicode.Scalar(_value: (rawValue << 14) >> 14)
    }

    // The index into the decomposition array is stored in the top 14 bits.
    var index: Int {
      Int(truncatingIfNeeded: rawValue >> 18)
    }

    // A buffer pointer to the UTF8 decomposition string.
    var utf8: UnsafeBufferPointer<UInt8> {
      let decompPtr = _swift_stdlib_nfd_decompositions._unsafelyUnwrappedUnchecked

      // This size is the utf8 length of the decomposition.
      let size = Int(truncatingIfNeeded: decompPtr[index])

      return UnsafeBufferPointer(
        // We add 1 here to skip the size byte.
        start: decompPtr + index + 1,
        count: size
      )
    }

    init(_ scalar: Unicode.Scalar) {
      rawValue = _swift_stdlib_getDecompositionEntry(scalar.value)
    }
  }
}
