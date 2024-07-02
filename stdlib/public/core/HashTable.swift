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

@usableFromInline
internal protocol _HashTableDelegate {
  func hashValue(at bucket: _HashTable.Bucket) -> Int
  func moveEntry(from source: _HashTable.Bucket, to target: _HashTable.Bucket)
}

@usableFromInline
@frozen
internal struct _HashTable {
  @usableFromInline
  internal typealias Word = _UnsafeBitset.Word

  @usableFromInline
  internal var words: UnsafeMutablePointer<Word>

  @usableFromInline
  internal let bucketMask: Int

  @inlinable
  @inline(__always)
  internal init(words: UnsafeMutablePointer<Word>, bucketCount: Int) {
    _internalInvariant(bucketCount > 0 && bucketCount & (bucketCount - 1) == 0,
      "bucketCount must be a power of two")
    self.words = words
    // The bucket count is a power of two, so subtracting 1 will never overflow
    // and get us a nice mask.
    self.bucketMask = bucketCount &- 1
  }

  @inlinable
  internal var bucketCount: Int {
    @inline(__always) get {
      return _assumeNonNegative(bucketMask &+ 1)
    }
  }

  @inlinable
  internal var wordCount: Int {
    @inline(__always) get {
      return _UnsafeBitset.wordCount(forCapacity: bucketCount)
    }
  }

  /// Return a bitset representation of the occupied buckets in this table.
  ///
  /// Note that if we have only a single partial word in the hash table's
  /// bitset, then its out-of-bounds bits are guaranteed to be all set. These
  /// filler bits are there to speed up finding holes -- they don't correspond
  /// to occupied buckets in the table.
  @_alwaysEmitIntoClient
  internal var bitset: _UnsafeBitset {
    _UnsafeBitset(words: words, wordCount: wordCount)
  }
}

@available(*, unavailable)
extension _HashTable: Sendable {}

extension _HashTable {
  /// The inverse of the maximum hash table load factor.
  private static var maxLoadFactor: (numerator: Int, denominator: Int) {
    @inline(__always) get { return (numerator: 3, denominator: 4) } // 3/4
  }

  internal static func capacity(forScale scale: Int8) -> Int {
    let bucketCount = (1 as Int) &<< scale
    return bucketCount * maxLoadFactor.numerator / maxLoadFactor.denominator
  }

  internal static func scale(forCapacity capacity: Int) -> Int8 {
    let capacity = Swift.max(capacity, 1)
    // Calculate the minimum number of entries we need to allocate to satisfy
    // the maximum load factor. `capacity + 1` below ensures that we always
    // leave at least one hole.
    func divideRoundingUp(n: Int, by: Int) -> Int { (n + (by - 1)) / by }
    let minimumEntries = Swift.max(divideRoundingUp(
      n: capacity * maxLoadFactor.denominator, by: maxLoadFactor.numerator),
      capacity + 1)
    // The actual number of entries we need to allocate is the lowest power of
    // two greater than or equal to the minimum entry count. Calculate its
    // exponent.
    let exponent = (Swift.max(minimumEntries, 2) - 1)._binaryLogarithm() + 1
    _internalInvariant(exponent >= 0 && exponent < Int.bitWidth)
    // The scale is the exponent corresponding to the bucket count.
    let scale = Int8(truncatingIfNeeded: exponent)
    _internalInvariant(self.capacity(forScale: scale) >= capacity)
    return scale
  }

  // The initial age to use for native copies of a Cocoa NSSet/NSDictionary.
  internal static func age(for cocoa: AnyObject) -> Int32 {
    let hash = ObjectIdentifier(cocoa).hashValue
    return Int32(truncatingIfNeeded: hash)
  }

  internal static func hashSeed(
    for object: Builtin.NativeObject,
    scale: Int8
  ) -> Int {
    // We generate a new hash seed whenever a new hash table is allocated and
    // whenever an existing table is resized, so that we avoid certain copy
    // operations becoming quadratic. For background details, see
    // https://github.com/apple/swift/issues/45856.
    //
    // Note that we do reuse the existing seed when making copy-on-write copies
    // so that we avoid breaking value semantics.
    if Hasher._isDeterministic {
      // When we're using deterministic hashing, the scale value as the seed is
      // still allowed, and it covers most cases. (Unfortunately some operations
      // that merge two similar-sized hash tables will still be quadratic.)
      return Int(scale)
    }
    // Use the object address as the hash seed. This is cheaper than
    // SystemRandomNumberGenerator, while it has the same practical effect.
    // Addresses aren't entirely random, but that's not the goal here -- the
    // 128-bit execution seed takes care of randomization. We only need to
    // guarantee that no two tables with the same seed can coexist at the same
    // time (apart from copy-on-write derivatives of the same table).
    return unsafeBitCast(object, to: Int.self)
  }
}

extension _HashTable {
  @frozen
  @usableFromInline
  internal struct Bucket {
    @usableFromInline
    internal var offset: Int

    @inlinable
    @inline(__always)
    internal init(offset: Int) {
      self.offset = offset
    }

    @inlinable
    @inline(__always)
    internal init(word: Int, bit: Int) {
      self.offset = _UnsafeBitset.join(word: word, bit: bit)
    }

    @inlinable
    internal var word: Int {
      @inline(__always) get {
        return _UnsafeBitset.word(for: offset)
      }
    }

    @inlinable
    internal var bit: Int {
      @inline(__always) get {
        return _UnsafeBitset.bit(for: offset)
      }
    }
  }
}

extension _HashTable.Bucket: Equatable {
  @inlinable
  @inline(__always)
  internal
  static func == (lhs: _HashTable.Bucket, rhs: _HashTable.Bucket) -> Bool {
    return lhs.offset == rhs.offset
  }
}

extension _HashTable.Bucket: Comparable {
  @inlinable
  @inline(__always)
  internal
  static func < (lhs: _HashTable.Bucket, rhs: _HashTable.Bucket) -> Bool {
    return lhs.offset < rhs.offset
  }
}

extension _HashTable {
  @usableFromInline
  @frozen
  internal struct Index {
    @usableFromInline
    let bucket: Bucket

    @usableFromInline
    let age: Int32

    @inlinable
    @inline(__always)
    internal init(bucket: Bucket, age: Int32) {
      self.bucket = bucket
      self.age = age
    }
  }
}

extension _HashTable.Index: Equatable {
  @inlinable
  @inline(__always)
  internal static func ==(
    lhs: _HashTable.Index,
    rhs: _HashTable.Index
  ) -> Bool {
    _precondition(lhs.age == rhs.age,
      "Can't compare indices belonging to different collections")
    return lhs.bucket == rhs.bucket
  }
}

extension _HashTable.Index: Comparable {
  @inlinable
  @inline(__always)
  internal static func <(
    lhs: _HashTable.Index,
    rhs: _HashTable.Index
  ) -> Bool {
    _precondition(lhs.age == rhs.age,
      "Can't compare indices belonging to different collections")
    return lhs.bucket < rhs.bucket
  }
}

extension _HashTable: Sequence {
  @usableFromInline
  @frozen
  internal struct Iterator: IteratorProtocol {
    @usableFromInline
    let hashTable: _HashTable
    @usableFromInline
    var wordIndex: Int
    @usableFromInline
    var word: Word

    @inlinable
    @inline(__always)
    init(_ hashTable: _HashTable) {
      self.hashTable = hashTable
      self.wordIndex = 0
      self.word = hashTable.words[0]
      if hashTable.bucketCount < Word.capacity {
        self.word = self.word.intersecting(elementsBelow: hashTable.bucketCount)
      }
    }

    @inlinable
    @inline(__always)
    internal mutating func next() -> Bucket? {
      if let bit = word.next() {
        return Bucket(word: wordIndex, bit: bit)
      }
      while wordIndex + 1 < hashTable.wordCount {
        wordIndex += 1
        word = hashTable.words[wordIndex]
        if let bit = word.next() {
          return Bucket(word: wordIndex, bit: bit)
        }
      }
      return nil
    }
  }

  @inlinable
  @inline(__always)
  internal func makeIterator() -> Iterator {
    return Iterator(self)
  }
}

@available(*, unavailable)
extension _HashTable.Iterator: Sendable {}

extension _HashTable {
  @inlinable
  @inline(__always)
  internal func isValid(_ bucket: Bucket) -> Bool {
    return bucket.offset >= 0 && bucket.offset < bucketCount
  }

  @inlinable
  @inline(__always)
  internal func _isOccupied(_ bucket: Bucket) -> Bool {
    _internalInvariant(isValid(bucket))
    return words[bucket.word].uncheckedContains(bucket.bit)
  }

  @inlinable
  @inline(__always)
  internal func isOccupied(_ bucket: Bucket) -> Bool {
    return isValid(bucket) && _isOccupied(bucket)
  }

  @inlinable
  @inline(__always)
  internal func checkOccupied(_ bucket: Bucket) {
    _precondition(isOccupied(bucket),
      "Attempting to access Collection elements using an invalid Index")
  }

  @inlinable
  @inline(__always)
  internal func _firstOccupiedBucket(fromWord word: Int) -> Bucket {
    _internalInvariant(word >= 0 && word <= wordCount)
    var word = word
    while word < wordCount {
      if let bit = words[word].minimum {
        return Bucket(word: word, bit: bit)
      }
      word += 1
    }
    return endBucket
  }

  @inlinable
  internal func occupiedBucket(after bucket: Bucket) -> Bucket {
    _internalInvariant(isValid(bucket))
    let word = bucket.word
    if let bit = words[word].intersecting(elementsAbove: bucket.bit).minimum {
      return Bucket(word: word, bit: bit)
    }
    return _firstOccupiedBucket(fromWord: word + 1)
  }

  @inlinable
  internal var startBucket: Bucket {
    return _firstOccupiedBucket(fromWord: 0)
  }

  @inlinable
  internal var endBucket: Bucket {
    @inline(__always)
    get {
      return Bucket(offset: bucketCount)
    }
  }
}

extension _HashTable {
  @inlinable
  @inline(__always)
  internal func idealBucket(forHashValue hashValue: Int) -> Bucket {
    return Bucket(offset: hashValue & bucketMask)
  }

  /// The next bucket after `bucket`, with wraparound at the end of the table.
  @inlinable
  @inline(__always)
  internal func bucket(wrappedAfter bucket: Bucket) -> Bucket {
    // The bucket is less than bucketCount, which is power of two less than
    // Int.max. Therefore adding 1 does not overflow.
    return Bucket(offset: (bucket.offset &+ 1) & bucketMask)
  }
}

extension _HashTable {
  @inlinable
  internal func previousHole(before bucket: Bucket) -> Bucket {
    _internalInvariant(isValid(bucket))
    // Note that if we have only a single partial word, its out-of-bounds bits
    // are guaranteed to be all set, so the formula below gives correct results.
    var word = bucket.word
    if let bit =
      words[word]
        .complement
        .intersecting(elementsBelow: bucket.bit)
        .maximum {
      return Bucket(word: word, bit: bit)
    }
    var wrap = false
    while true {
      word -= 1
      if word < 0 {
        _precondition(!wrap, "Hash table has no holes")
        wrap = true
        word = wordCount - 1
      }
      if let bit = words[word].complement.maximum {
        return Bucket(word: word, bit: bit)
      }
    }
  }

  @inlinable
  internal func nextHole(atOrAfter bucket: Bucket) -> Bucket {
    _internalInvariant(isValid(bucket))
    // Note that if we have only a single partial word, its out-of-bounds bits
    // are guaranteed to be all set, so the formula below gives correct results.
    var word = bucket.word
    if let bit =
      words[word]
        .complement
        .subtracting(elementsBelow: bucket.bit)
        .minimum {
      return Bucket(word: word, bit: bit)
    }
    var wrap = false
    while true {
      word &+= 1
      if word == wordCount {
        _precondition(!wrap, "Hash table has no holes")
        wrap = true
        word = 0
      }
      if let bit = words[word].complement.minimum {
        return Bucket(word: word, bit: bit)
      }
    }
  }
}

extension _HashTable {
  @inlinable
  @inline(__always)
  @_effects(releasenone)
  internal func copyContents(of other: _HashTable) {
    _internalInvariant(bucketCount == other.bucketCount)
    self.words.update(from: other.words, count: wordCount)
  }

  /// Insert a new entry with the specified hash value into the table.
  /// The entry must not already exist in the table -- duplicates are ignored.
  @inlinable
  @inline(__always)
  internal func insertNew(hashValue: Int) -> Bucket {
    let hole = nextHole(atOrAfter: idealBucket(forHashValue: hashValue))
    insert(hole)
    return hole
  }

  /// Insert a new entry for an element at `index`.
  @inlinable
  @inline(__always)
  internal func insert(_ bucket: Bucket) {
    _internalInvariant(!isOccupied(bucket))
    words[bucket.word].uncheckedInsert(bucket.bit)
  }

  @inlinable
  @inline(__always)
  internal func clear() {
    if bucketCount < Word.capacity {
      // We have only a single partial word. Set all out of bounds bits, so that
      // `occupiedBucket(after:)` and `nextHole(atOrAfter:)` works correctly
      // without a special case.
      words[0] = Word.allBits.subtracting(elementsBelow: bucketCount)
    } else {
      words.update(repeating: .empty, count: wordCount)
    }
  }

  @inline(__always)
  @inlinable
  internal func delete<D: _HashTableDelegate>(
    at bucket: Bucket,
    with delegate: D
  ) {
    _internalInvariant(isOccupied(bucket))

    // If we've put a hole in a chain of contiguous elements, some element after
    // the hole may belong where the new hole is.

    var hole = bucket
    var candidate = self.bucket(wrappedAfter: hole)

    guard _isOccupied(candidate) else {
      // Fast path: Don't get the first bucket when there's nothing to do.
      words[hole.word].uncheckedRemove(hole.bit)
      return
    }

    // Find the first bucket in the contiguous chain that contains the entry
    // we've just deleted.
    let start = self.bucket(wrappedAfter: previousHole(before: bucket))

    // Relocate out-of-place elements in the chain, repeating until we get to
    // the end of the chain.
    while _isOccupied(candidate) {
      let candidateHash = delegate.hashValue(at: candidate)
      let ideal = idealBucket(forHashValue: candidateHash)

      // Does this element belong between start and hole?  We need two
      // separate tests depending on whether [start, hole] wraps around the
      // end of the storage.
      let c0 = ideal >= start
      let c1 = ideal <= hole
      if start <= hole ? (c0 && c1) : (c0 || c1) {
        delegate.moveEntry(from: candidate, to: hole)
        hole = candidate
      }
      candidate = self.bucket(wrappedAfter: candidate)
    }

    words[hole.word].uncheckedRemove(hole.bit)
  }
}
