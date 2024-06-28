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

/// Initializes a `Dictionary` from unique members.
///
/// Using a builder can be faster than inserting members into an empty
/// `Dictionary`.
@frozen
public // SPI(Foundation)
struct _DictionaryBuilder<Key: Hashable, Value> {
  @usableFromInline
  internal var _target: _NativeDictionary<Key, Value>
  @usableFromInline
  internal let _requestedCount: Int

  @inlinable
  public init(count: Int) {
    _target = _NativeDictionary(capacity: count)
    _requestedCount = count
  }

  @inlinable
  public mutating func add(key newKey: Key, value: Value) {
    _precondition(_target.count < _requestedCount,
      "Can't add more members than promised")
    _target._unsafeInsertNew(key: newKey, value: value)
  }

  @inlinable
  public __consuming func take() -> Dictionary<Key, Value> {
    _precondition(_target.count == _requestedCount,
      "The number of members added does not match the promised count")
    return Dictionary(_native: _target)
  }
}

@available(*, unavailable)
extension _DictionaryBuilder: Sendable {}

extension Dictionary {
  /// Creates a new dictionary with the specified capacity, then calls the given
  /// closure to initialize its contents.
  ///
  /// Foundation uses this initializer to bridge the contents of an NSDictionary
  /// instance without allocating a pair of intermediary buffers.  Pass the
  /// required capacity and a closure that can initialize the dictionary's
  /// elements. The closure must return `c`, the number of initialized elements
  /// in both buffers, such that the elements in the range `0..<c` are
  /// initialized and the elements in the range `c..<capacity` are
  /// uninitialized.
  ///
  /// The resulting dictionary has a `count` less than or equal to `c`.
  /// If some of the initialized keys were duplicates, the actual count is less.
  /// This cannot happen for any other reasons or if `allowingDuplicates` is false.
  ///
  /// The buffers passed to the closure are only valid for the duration of the
  /// call.  After the closure returns, this initializer moves all initialized
  /// elements into their correct buckets.
  ///
  /// - Parameters:
  ///   - capacity: The capacity of the new dictionary.
  ///   - allowingDuplicates: If false, then the caller guarantees that all keys
  ///     are unique. This promise isn't verified -- if it turns out to be
  ///     false, then the resulting dictionary won't be valid.
  ///   - body: A closure that can initialize the dictionary's elements. This
  ///     closure must return the count of the initialized elements, starting at
  ///     the beginning of the buffer.
  @_alwaysEmitIntoClient // Introduced in 5.1
  public // SPI(Foundation)
  init(
    _unsafeUninitializedCapacity capacity: Int,
    allowingDuplicates: Bool,
    initializingWith initializer: (
      _ keys: UnsafeMutableBufferPointer<Key>,
      _ values: UnsafeMutableBufferPointer<Value>
    ) -> Int
  ) {
    self.init(_native: _NativeDictionary(
        _unsafeUninitializedCapacity: capacity,
        allowingDuplicates: allowingDuplicates,
        initializingWith: initializer))
  }
}

extension _NativeDictionary {
  @_alwaysEmitIntoClient // Introduced in 5.1
  internal init(
    _unsafeUninitializedCapacity capacity: Int,
    allowingDuplicates: Bool,
    initializingWith initializer: (
      _ keys: UnsafeMutableBufferPointer<Key>,
      _ values: UnsafeMutableBufferPointer<Value>
    ) -> Int
  ) {
    self.init(capacity: capacity)

    // If the capacity is 0, then our storage is the empty singleton. Those are
    // read only, so we shouldn't attempt to write to them.
    if capacity == 0 {
      let c = initializer(
        UnsafeMutableBufferPointer(start: nil, count: 0), 
        UnsafeMutableBufferPointer(start: nil, count: 0))
      _precondition(c == 0)
      return
    }

    let initializedCount = initializer(
      UnsafeMutableBufferPointer(start: _keys, count: capacity),
      UnsafeMutableBufferPointer(start: _values, count: capacity))
    _precondition(initializedCount >= 0 && initializedCount <= capacity)
    _storage._count = initializedCount

    // Hash initialized elements and move each of them into their correct
    // buckets.
    //
    // - We have some number of unprocessed elements at the start of the
    //   key/value buffers -- buckets up to and including `bucket`. Everything
    //   in this region is either unprocessed or in use. There are no
    //   uninitialized entries in it.
    //
    // - Everything after `bucket` is either uninitialized or in use. This
    //   region works exactly like regular dictionary storage.
    //
    // - "in use" is tracked by the bitmap in `hashTable`, the same way it would
    //   be for a working Dictionary.
    //
    // Each iteration of the loop below processes an unprocessed element, and/or
    // reduces the size of the unprocessed region, while ensuring the above
    // invariants.
    var bucket = _HashTable.Bucket(offset: initializedCount - 1)
    while bucket.offset >= 0 {
      if hashTable._isOccupied(bucket) {
        // We've moved an element here in a previous iteration.
        bucket.offset -= 1
        continue
      }
      // Find the target bucket for this entry and mark it as in use.
      let target: Bucket
      if _isDebugAssertConfiguration() || allowingDuplicates {
        let (b, found) = find(_keys[bucket.offset])
        if found {
          _internalInvariant(b != bucket)
          _precondition(allowingDuplicates, "Duplicate keys found")
          // Discard duplicate entry.
          uncheckedDestroy(at: bucket)
          _storage._count -= 1
          bucket.offset -= 1
          continue
        }
        hashTable.insert(b)
        target = b
      } else {
        let hashValue = self.hashValue(for: _keys[bucket.offset])
        target = hashTable.insertNew(hashValue: hashValue)
      }

      if target > bucket {
        // The target is outside the unprocessed region.  We can simply move the
        // entry, leaving behind an uninitialized bucket.
        moveEntry(from: bucket, to: target)
        // Restore invariants by lowering the region boundary.
        bucket.offset -= 1
      } else if target == bucket {
        // Already in place.
        bucket.offset -= 1
      } else {
        // The target bucket is also in the unprocessed region. Swap the current
        // item into place, then try again with the swapped-in value, so that we
        // don't lose it.
        swapEntry(target, with: bucket)
      }
    }
    // When there are no more unprocessed entries, we're left with a valid
    // Dictionary.
  }
}
