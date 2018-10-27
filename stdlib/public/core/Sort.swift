//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//


//===----------------------------------------------------------------------===//
// sorted()/sort()
//===----------------------------------------------------------------------===//

extension Sequence where Element: Comparable {
  /// Returns the elements of the sequence, sorted.
  ///
  /// You can sort any sequence of elements that conform to the `Comparable`
  /// protocol by calling this method. Elements are sorted in ascending order.
  ///
  /// The sorting algorithm is not stable. A nonstable sort may change the
  /// relative order of elements that compare equal.
  ///
  /// Here's an example of sorting a list of students' names. Strings in Swift
  /// conform to the `Comparable` protocol, so the names are sorted in
  /// ascending order according to the less-than operator (`<`).
  ///
  ///     let students: Set = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     let sortedStudents = students.sorted()
  ///     print(sortedStudents)
  ///     // Prints "["Abena", "Akosua", "Kofi", "Kweku", "Peter"]"
  ///
  /// To sort the elements of your sequence in descending order, pass the
  /// greater-than operator (`>`) to the `sorted(by:)` method.
  ///
  ///     let descendingStudents = students.sorted(by: >)
  ///     print(descendingStudents)
  ///     // Prints "["Peter", "Kweku", "Kofi", "Akosua", "Abena"]"
  ///
  /// - Returns: A sorted array of the sequence's elements.
  ///
  /// - Complexity: O(*n* log *n*), where *n* is the length of the sequence.
  @inlinable
  public func sorted() -> [Element] {
    return sorted(by: <)
  }
}

extension Sequence {
  /// Returns the elements of the sequence, sorted using the given predicate as
  /// the comparison between elements.
  ///
  /// When you want to sort a sequence of elements that don't conform to the
  /// `Comparable` protocol, pass a predicate to this method that returns
  /// `true` when the first element passed should be ordered before the
  /// second. The elements of the resulting array are ordered according to the
  /// given predicate.
  ///
  /// The predicate must be a *strict weak ordering* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areInIncreasingOrder(a, a)` is always `false`. (Irreflexivity)
  /// - If `areInIncreasingOrder(a, b)` and `areInIncreasingOrder(b, c)` are
  ///   both `true`, then `areInIncreasingOrder(a, c)` is also `true`.
  ///   (Transitive comparability)
  /// - Two elements are *incomparable* if neither is ordered before the other
  ///   according to the predicate. If `a` and `b` are incomparable, and `b`
  ///   and `c` are incomparable, then `a` and `c` are also incomparable.
  ///   (Transitive incomparability)
  ///
  /// The sorting algorithm is not stable. A nonstable sort may change the
  /// relative order of elements for which `areInIncreasingOrder` does not
  /// establish an order.
  ///
  /// In the following example, the predicate provides an ordering for an array
  /// of a custom `HTTPResponse` type. The predicate orders errors before
  /// successes and sorts the error responses by their error code.
  ///
  ///     enum HTTPResponse {
  ///         case ok
  ///         case error(Int)
  ///     }
  ///
  ///     let responses: [HTTPResponse] = [.error(500), .ok, .ok, .error(404), .error(403)]
  ///     let sortedResponses = responses.sorted {
  ///         switch ($0, $1) {
  ///         // Order errors by code
  ///         case let (.error(aCode), .error(bCode)):
  ///             return aCode < bCode
  ///
  ///         // All successes are equivalent, so none is before any other
  ///         case (.ok, .ok): return false
  ///
  ///         // Order errors before successes
  ///         case (.error, .ok): return true
  ///         case (.ok, .error): return false
  ///         }
  ///     }
  ///     print(sortedResponses)
  ///     // Prints "[.error(403), .error(404), .error(500), .ok, .ok]"
  ///
  /// You also use this method to sort elements that conform to the
  /// `Comparable` protocol in descending order. To sort your sequence in
  /// descending order, pass the greater-than operator (`>`) as the
  /// `areInIncreasingOrder` parameter.
  ///
  ///     let students: Set = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     let descendingStudents = students.sorted(by: >)
  ///     print(descendingStudents)
  ///     // Prints "["Peter", "Kweku", "Kofi", "Akosua", "Abena"]"
  ///
  /// Calling the related `sorted()` method is equivalent to calling this
  /// method and passing the less-than operator (`<`) as the predicate.
  ///
  ///     print(students.sorted())
  ///     // Prints "["Abena", "Akosua", "Kofi", "Kweku", "Peter"]"
  ///     print(students.sorted(by: <))
  ///     // Prints "["Abena", "Akosua", "Kofi", "Kweku", "Peter"]"
  ///
  /// - Parameter areInIncreasingOrder: A predicate that returns `true` if its
  ///   first argument should be ordered before its second argument;
  ///   otherwise, `false`.
  /// - Returns: A sorted array of the sequence's elements.
  ///
  /// - Complexity: O(*n* log *n*), where *n* is the length of the sequence.
  @inlinable
  public func sorted(
    by areInIncreasingOrder:
      (Element, Element) throws -> Bool
  ) rethrows -> [Element] {
    var result = ContiguousArray(self)
    try result.sort(by: areInIncreasingOrder)
    return Array(result)
  }
}

extension MutableCollection
where Self: RandomAccessCollection, Element: Comparable {
  /// Sorts the collection in place.
  ///
  /// You can sort any mutable collection of elements that conform to the
  /// `Comparable` protocol by calling this method. Elements are sorted in
  /// ascending order.
  ///
  /// The sorting algorithm is not stable. A nonstable sort may change the
  /// relative order of elements that compare equal.
  ///
  /// Here's an example of sorting a list of students' names. Strings in Swift
  /// conform to the `Comparable` protocol, so the names are sorted in
  /// ascending order according to the less-than operator (`<`).
  ///
  ///     var students = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     students.sort()
  ///     print(students)
  ///     // Prints "["Abena", "Akosua", "Kofi", "Kweku", "Peter"]"
  ///
  /// To sort the elements of your collection in descending order, pass the
  /// greater-than operator (`>`) to the `sort(by:)` method.
  ///
  ///     students.sort(by: >)
  ///     print(students)
  ///     // Prints "["Peter", "Kweku", "Kofi", "Akosua", "Abena"]"
  ///
  /// - Complexity: O(*n* log *n*), where *n* is the length of the collection.
  @inlinable
  public mutating func sort() {
    sort(by: <)
  }
}

extension MutableCollection where Self: RandomAccessCollection {
  /// Sorts the collection in place, using the given predicate as the
  /// comparison between elements.
  ///
  /// When you want to sort a collection of elements that doesn't conform to
  /// the `Comparable` protocol, pass a closure to this method that returns
  /// `true` when the first element passed should be ordered before the
  /// second.
  ///
  /// The predicate must be a *strict weak ordering* over the elements. That
  /// is, for any elements `a`, `b`, and `c`, the following conditions must
  /// hold:
  ///
  /// - `areInIncreasingOrder(a, a)` is always `false`. (Irreflexivity)
  /// - If `areInIncreasingOrder(a, b)` and `areInIncreasingOrder(b, c)` are
  ///   both `true`, then `areInIncreasingOrder(a, c)` is also `true`.
  ///   (Transitive comparability)
  /// - Two elements are *incomparable* if neither is ordered before the other
  ///   according to the predicate. If `a` and `b` are incomparable, and `b`
  ///   and `c` are incomparable, then `a` and `c` are also incomparable.
  ///   (Transitive incomparability)
  ///
  /// The sorting algorithm is not stable. A nonstable sort may change the
  /// relative order of elements for which `areInIncreasingOrder` does not
  /// establish an order.
  ///
  /// In the following example, the closure provides an ordering for an array
  /// of a custom enumeration that describes an HTTP response. The predicate
  /// orders errors before successes and sorts the error responses by their
  /// error code.
  ///
  ///     enum HTTPResponse {
  ///         case ok
  ///         case error(Int)
  ///     }
  ///
  ///     var responses: [HTTPResponse] = [.error(500), .ok, .ok, .error(404), .error(403)]
  ///     responses.sort {
  ///         switch ($0, $1) {
  ///         // Order errors by code
  ///         case let (.error(aCode), .error(bCode)):
  ///             return aCode < bCode
  ///
  ///         // All successes are equivalent, so none is before any other
  ///         case (.ok, .ok): return false
  ///
  ///         // Order errors before successes
  ///         case (.error, .ok): return true
  ///         case (.ok, .error): return false
  ///         }
  ///     }
  ///     print(responses)
  ///     // Prints "[.error(403), .error(404), .error(500), .ok, .ok]"
  ///
  /// Alternatively, use this method to sort a collection of elements that do
  /// conform to `Comparable` when you want the sort to be descending instead
  /// of ascending. Pass the greater-than operator (`>`) operator as the
  /// predicate.
  ///
  ///     var students = ["Kofi", "Abena", "Peter", "Kweku", "Akosua"]
  ///     students.sort(by: >)
  ///     print(students)
  ///     // Prints "["Peter", "Kweku", "Kofi", "Akosua", "Abena"]"
  ///
  /// - Parameter areInIncreasingOrder: A predicate that returns `true` if its
  ///   first argument should be ordered before its second argument;
  ///   otherwise, `false`. If `areInIncreasingOrder` throws an error during
  ///   the sort, the elements may be in a different order, but none will be
  ///   lost.
  ///
  /// - Complexity: O(*n* log *n*), where *n* is the length of the collection.
  @inlinable
  public mutating func sort(
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    let didSortUnsafeBuffer = try _withUnsafeMutableBufferPointerIfSupported {
      buffer -> Void? in
        try buffer._stableSortImpl(by: areInIncreasingOrder)
    }
    if didSortUnsafeBuffer == nil {
      try _stableSortImpl(by: areInIncreasingOrder)
    }
  }
}

extension MutableCollection where Self: BidirectionalCollection {
  /// Sorts `self[range]` according to `areInIncreasingOrder`. Stable.
  ///
  /// - Precondition: `sortedEnd != range.lowerBound`
  /// - Precondition: `elements[..<sortedEnd]` are already in order.
  @inlinable
  internal mutating func _insertionSort(
    within range: Range<Index>,
    sortedEnd: Index,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    var sortedEnd = sortedEnd
    
    // Continue sorting until the sorted elements cover the whole sequence.
    while sortedEnd != range.upperBound {
      var i = sortedEnd
      // Look backwards for `self[i]`'s position in the sorted sequence,
      // moving each element forward to make room.
      repeat {
        let j = index(before: i)
        
        // If `self[i]` doesn't belong before `self[j]`, we've found
        // its position.
        if try !areInIncreasingOrder(self[i], self[j]) {
          break
        }
        
        // Swap the elements at `i` and `j`.
        swapAt(i, j)
        i = j
      } while i != range.lowerBound
      
      formIndex(after: &sortedEnd)
    }
  }
  
  /// Sorts `self[range]` according to `areInIncreasingOrder`. Stable.
  @inlinable
  internal mutating func _insertionSort(
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    if range.isEmpty {
      return
    }
    
    // One element is trivially already-sorted, so the actual sort can
    // start on the second element.
    let sortedEnd = index(after: range.lowerBound)
    try _insertionSort(
      within: range, sortedEnd: sortedEnd, by: areInIncreasingOrder)
  }
  
  /// Reverses the elements in the given range.
  @inlinable
  internal mutating func _reverse(
    within range: Range<Index>
  ) {
    var f = range.lowerBound
    var l = range.upperBound
    while f < l {
      formIndex(before: &l)
      swapAt(f, l)
      formIndex(after: &f)
    }
  }
}

/// Merges the elements in the ranges `lo..<mid` and `mid..<hi` using `buffer`
/// as out-of-place storage. Stable.
///
/// - Precondition: `lo..<mid` and `mid..<hi` must already be sorted according
///   to `areInIncreasingOrder`.
/// - Precondition: `buffer` must point to a region of memory at least as large
///   as `min(mid - lo, hi - mid)`.
/// - Postcondition: `lo..<hi` is sorted according to `areInIncreasingOrder`.
@inlinable
internal func _merge<Element>(
  low: UnsafeMutablePointer<Element>,
  mid: UnsafeMutablePointer<Element>,
  high: UnsafeMutablePointer<Element>,
  buffer: UnsafeMutablePointer<Element>,
  by areInIncreasingOrder: (Element, Element) throws -> Bool
) rethrows {
  let lowCount = mid - low
  let highCount = high - mid
  
  var destLow = low         // Lower bound of uninitialized storage
  var bufferLow = buffer    // Lower bound of the initialized buffer
  var bufferHigh = buffer   // Upper bound of the initialized buffer

  // When we exit the merge, move any remaining elements from the buffer back
  // into `destLow` (aka the collection we're sorting). The buffer can have
  // remaining elements if `areIncreasingOrder` throws, or more likely if the
  // merge runs out of elements from the array before exhausting the buffer.
  defer {
    destLow.moveInitialize(from: bufferLow, count: bufferHigh - bufferLow)
  }
  
  if lowCount < highCount {
    // Move the lower group of elements into the buffer, then traverse from
    // low to high in both the buffer and the higher group of elements.
    //
    // After moving elements, the storage and buffer look like this, where
    // `x` is uninitialized memory:
    //
    // Storage: [x, x, x, x, x, 6, 8, 8, 10, 12, 15]
    //           ^              ^
    //        destLow        srcLow
    //
    // Buffer:  [4, 4, 7, 8, 9, x, ...]
    //           ^              ^
    //        bufferLow     bufferHigh
    buffer.moveInitialize(from: low, count: lowCount)
    bufferHigh = bufferLow + lowCount
    
    var srcLow = mid

    // Each iteration moves the element that compares lower into `destLow`,
    // preferring the buffer when equal to maintain stability. Elements are
    // moved from either `bufferLow` or `srcLow`, with those pointers
    // incrementing as elements are moved.
    while bufferLow < bufferHigh && srcLow < high {
      if try areInIncreasingOrder(srcLow.pointee, bufferLow.pointee) {
        destLow.moveInitialize(from: srcLow, count: 1)
        srcLow += 1
      } else {
        destLow.moveInitialize(from: bufferLow, count: 1)
        bufferLow += 1
      }
      destLow += 1
    }
  } else {
    // Move the higher group of elements into the buffer, then traverse from
    // high to low in both the buffer and the lower group of elements.
    //
    // After moving elements, the storage and buffer look like this, where
    // `x` is uninitialized memory:
    //
    // Storage: [4, 4, 7, 8, 9, 6, x, x,  x,  x,  x]
    //                          ^  ^                 ^
    //                    srcHigh  destLow        destHigh (past the end)
    //
    // Buffer:                    [8, 8, 10, 12, 15, x, ...]
    //                             ^                 ^
    //                          bufferLow        bufferHigh
    buffer.moveInitialize(from: mid, count: highCount)
    bufferHigh = bufferLow + highCount
    
    var destHigh = high
    var srcHigh = mid
    destLow = mid

    // Each iteration moves the element that compares higher into `destHigh`,
    // preferring the buffer when equal to maintain stability. Elements are
    // moved from either `bufferHigh - 1` or `srcHigh - 1`, with those
    // pointers decrementing as elements are moved.
    //
    // Note: At the start of each iteration, each `...High` pointer points one
    // past the element they're referring to.
    while bufferHigh > bufferLow && srcHigh > low {
      destHigh -= 1
      if try areInIncreasingOrder(
        (bufferHigh - 1).pointee, (srcHigh - 1).pointee
      ) {
        srcHigh -= 1
        destHigh.moveInitialize(from: srcHigh, count: 1)
        
        // Moved an element from the lower initialized portion to the upper,
        // sorted, initialized portion, so `destLow` moves down one.
        destLow -= 1
      } else {
        bufferHigh -= 1
        destHigh.moveInitialize(from: bufferHigh, count: 1)
      }
    }
  }
}

/// Calculates an optimal minimum run length for sorting a collection.
///
/// "... pick a minrun in range(32, 65) such that N/minrun is exactly a power
/// of 2, or if that isn't possible, is close to, but strictly less than, a
/// power of 2. This is easier to do than it may sound: take the first 6 bits
/// of N, and add 1 if any of the remaining bits are set."
/// - From the Timsort introduction, at
///   https://svn.python.org/projects/python/trunk/Objects/listsort.txt
///
/// - Parameter c: The number of elements in a collection.
/// - Returns: If `c <= 64`, returns `c`. Otherwise, returns a value in
///   `32...64`.
@inlinable
internal func _minimumMergeRunLength(_ c: Int) -> Int {
  // Max out at `2^6 == 64` elements
  let bitsToUse = 6
  
  if c < 1 << bitsToUse {
    return c
  }
  let offset = (MemoryLayout<Int>.size * 8 - bitsToUse) - c.leadingZeroBitCount
  let mask = (1 << offset) - 1
  return c >> offset + (c & mask == 0 ? 0 : 1)
}

/// Returns the end of the next in-order run along with a Boolean value
/// indicating whether the elements in `start..<end` are in increasing order.
@inlinable
internal func _findNextRun<C: RandomAccessCollection>(
  in elements: C,
  from start: C.Index,
  by areInIncreasingOrder: (C.Element, C.Element) throws -> Bool
) rethrows -> (end: C.Index, ascending: Bool) {
  var next = start
  if next < elements.endIndex {
    var current = next
    elements.formIndex(after: &next)
    if try next < elements.endIndex &&
      areInIncreasingOrder(elements[next], elements[current])
    {
      // The elements of this run are in descending order. Equal elements
      // cannot be included in the run, since the run will be reversed, which
      // would break stability.
      repeat {
        current = next
        elements.formIndex(after: &next)
      } while try next < elements.endIndex &&
        areInIncreasingOrder(elements[next], elements[current])
      
      return(next, false)
    } else if next < elements.endIndex {
      // The elements in this run are in ascending order. Equal elements can
      // be included in the run.
      repeat {
        current = next
        elements.formIndex(after: &next)
      } while try next < elements.endIndex &&
        !areInIncreasingOrder(elements[next], elements[current])
    }
  }
  return (next, true)
}

extension UnsafeMutableBufferPointer {
  /// Merges the elements at `runs[i]` and `runs[i - 1]`, using `buffer` as
  /// out-of-place storage.
  ///
  /// - Precondition: `runs.count > 1` and `i > 0`
  /// - Precondition: `buffer` must have at least
  ///   `min(runs[i].count, runs[i - 1].count)` uninitialized elements.
  @inlinable
  public mutating func _mergeRuns(
    _ runs: inout [Range<Index>],
    at i: Int,
    buffer: UnsafeMutablePointer<Element>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    precondition(i > 0)
    let low = runs[i - 1].lowerBound
    precondition(runs[i - 1].upperBound == runs[i].lowerBound)
    let middle = runs[i].lowerBound
    let high = runs[i].upperBound
    
    try _merge(
      low: baseAddress! + low,
      mid: baseAddress! + middle,
      high: baseAddress! + high,
      buffer: buffer,
      by: areInIncreasingOrder)
    
    runs[i - 1] = low..<high
    runs.remove(at: i)
  }
  
  /// Merges upper elements of `runs` until the required invariants are
  /// satisfied.
  ///
  /// - Precondition: `buffer` must have at least
  ///   `min(runs[i].count, runs[i - 1].count)` uninitialized elements.
  /// - Precondition: The ranges in `runs` must be consecutive, such that for
  ///   any i, `runs[i].upperBound == runs[i + 1].lowerBound`.
  @inlinable
  public mutating func _mergeTopRuns(
    _ runs: inout [Range<Index>],
    buffer: UnsafeMutablePointer<Element>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    // The invariants for the `runs` array are:
    // (a) - for all i in 2..<runs.count:
    //         - runs[i - 2].count > runs[i - 1].count + runs[i].count
    // (b) - for c = runs.count - 1:
    //         - runs[i - 1].count > runs[i].count
    //
    // Loop until the invariant is satisified for the top four elements of
    // `runs`. Because this method is called for every added run, and only
    // the top three runs are ever merged, this guarantees the invariant holds
    // for the whole array.
    //
    // At all times, `runs` is one of the following, where W, X, Y, and Z are
    // the counts of their respective ranges:
    // - [ ...?, W, X, Y, Z ]
    // - [ X, Y, Z ]
    // - [ Y, Z ]
    //
    // If W > X + Y, X > Y + Z, and Y > Z, then the invariants are satisfied
    // for the entirety of `runs`.
    
    // The invariant is always in place for a single element.
    while runs.count > 1 {
      var lastIndex = runs.count - 1
      
      if lastIndex >= 3 &&
        (runs[lastIndex - 3].count <=
          runs[lastIndex - 2].count + runs[lastIndex - 1].count)
      {
        // Second-to-last three runs do not follow W > X + Y.
        // Always merge Y with the smaller of X or Z.
        if runs[lastIndex - 2].count < runs[lastIndex].count {
          lastIndex -= 1
        }
      } else if lastIndex >= 2 &&
        (runs[lastIndex - 2].count <=
          runs[lastIndex - 1].count + runs[lastIndex].count)
      {
        // Last three runs do not follow X > Y + Z.
        // Always merge Y with the smaller of X or Z.
        if runs[lastIndex - 2].count < runs[lastIndex].count {
          lastIndex -= 1
        }
      } else if runs[lastIndex - 1].count <= runs[lastIndex].count {
        // Last two runs do not follow Y > Z.
        // Merge Y and Z.
      } else {
        // All invariants satisfied!
        break
      }
      
      // Merge the runs at `i` and `i - 1`.
      try _mergeRuns(
        &runs, at: lastIndex, buffer: buffer, by: areInIncreasingOrder)
    }
  }
  
  /// Merges elements of `runs` until only one run remains.
  ///
  /// - Precondition: `buffer` must have at least
  ///   `min(runs[i].count, runs[i - 1].count)` uninitialized elements.
  /// - Precondition: The ranges in `runs` must be consecutive, such that for
  ///   any i, `runs[i].upperBound == runs[i + 1].lowerBound`.
  @inlinable
  public mutating func _finalizeRuns(
    _ runs: inout [Range<Index>],
    buffer: UnsafeMutablePointer<Element>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    while runs.count > 1 {
      try _mergeRuns(
        &runs, at: runs.count - 1, buffer: buffer, by: areInIncreasingOrder)
    }
  }
  
  /// Sorts the elements of this buffer according to `areInIncreasingOrder`,
  /// using a stable, adaptive merge sort.
  ///
  /// The adaptive algorithm used is Timsort, modified to perform a straight
  /// merge of the elements using a temporary buffer.
  @inlinable
  public mutating func _stableSortImpl(
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    let minimumRunLength = _minimumMergeRunLength(count)
    if count <= minimumRunLength {
      try _insertionSort(
        within: startIndex..<endIndex, by: areInIncreasingOrder)
      return
    }
    
    // Use array's allocating initializer to create a temporary buffer---this
    // keeps the buffer allocation going through the same tail-allocated path
    // as other allocating methods.
    //
    // There's no need to set the initialized count within the initializing
    // closure, since the buffer is guaranteed to be uninitialized at exit.
    _ = try Array<Element>(_unsafeUninitializedCapacity: count / 2) {
      buffer, _ in
      var runs: [Range<Index>] = []
      
      var start = startIndex
      while start < endIndex {
        // Find the next consecutive run, reversing it if necessary.
        var (end, ascending) =
          try _findNextRun(in: self, from: start, by: areInIncreasingOrder)
        if !ascending {
          _reverse(within: start..<end)
        }
        
        // If the current run is shorter than the minimum length, use the
        // insertion sort to extend it.
        if end < endIndex && end - start < minimumRunLength {
          let newEnd = Swift.min(endIndex, start + minimumRunLength)
          try _insertionSort(
            within: start..<newEnd, sortedEnd: end, by: areInIncreasingOrder)
          end = newEnd
        }
        
        // Append this run and merge down as needed to maintain the `runs`
        // invariants.
        runs.append(start..<end)
        try _mergeTopRuns(
          &runs, buffer: buffer.baseAddress!, by: areInIncreasingOrder)
        start = end
      }
      
      try _finalizeRuns(
        &runs, buffer: buffer.baseAddress!, by: areInIncreasingOrder)
      assert(runs.count == 1, "Didn't complete final merge")
    }
  }
}

extension MutableCollection {
  /// This unconstrained default implementation is a fallback stable sort that
  /// sorts into an outside array, then copies elements back in.
  @inlinable
  public mutating func _stableSortImpl(
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    let sortedElements = try sorted(by: areInIncreasingOrder)
    for (i, j) in zip(indices, sortedElements.indices) {
      self[i] = sortedElements[j]
    }
  }
}

extension MutableCollection {
  /// Sorts the elements at `elements[a]`, `elements[b]`, and `elements[c]`.
  /// Stable.
  ///
  /// The indices passed as `a`, `b`, and `c` do not need to be consecutive, but
  /// must be in strict increasing order.
  ///
  /// - Precondition: `a < b && b < c`
  /// - Postcondition: `self[a] <= self[b] && self[b] <= self[c]`
  @inlinable
  public // @testable
  mutating func _sort3(
    _ a: Index, _ b: Index, _ c: Index, 
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    // There are thirteen possible permutations for the original ordering of
    // the elements at indices `a`, `b`, and `c`. The comments in the code below
    // show the relative ordering of the three elements using a three-digit
    // number as shorthand for the position and comparative relationship of
    // each element. For example, "312" indicates that the element at `a` is the
    // largest of the three, the element at `b` is the smallest, and the element
    // at `c` is the median. This hypothetical input array has a 312 ordering for
    // `a`, `b`, and `c`:
    //
    //      [ 7, 4, 3, 9, 2, 0, 3, 7, 6, 5 ]
    //        ^              ^           ^
    //        a              b           c
    //
    // - If each of the three elements is distinct, they could be ordered as any
    //   of the permutations of 1, 2, and 3: 123, 132, 213, 231, 312, or 321.
    // - If two elements are equivalent and one is distinct, they could be
    //   ordered as any permutation of 1, 1, and 2 or 1, 2, and 2: 112, 121, 211,
    //   122, 212, or 221.
    // - If all three elements are equivalent, they are already in order: 111.

    switch try (areInIncreasingOrder(self[b], self[a]),
                areInIncreasingOrder(self[c], self[b])) {
    case (false, false):
      // 0 swaps: 123, 112, 122, 111
      break

    case (true, true):
      // 1 swap: 321
      // swap(a, c): 312->123
      swapAt(a, c)

    case (true, false):
      // 1 swap: 213, 212 --- 2 swaps: 312, 211
      // swap(a, b): 213->123, 212->122, 312->132, 211->121
      swapAt(a, b)

      if try areInIncreasingOrder(self[c], self[b]) {
        // 132 (started as 312), 121 (started as 211)
        // swap(b, c): 132->123, 121->112
        swapAt(b, c)
      }

    case (false, true):
      // 1 swap: 132, 121 --- 2 swaps: 231, 221
      // swap(b, c): 132->123, 121->112, 231->213, 221->212
      swapAt(b, c)

      if try areInIncreasingOrder(self[b], self[a]) {
        // 213 (started as 231), 212 (started as 221)
        // swap(a, b): 213->123, 212->122
        swapAt(a, b)
      }
    }
  }
}

extension MutableCollection where Self: RandomAccessCollection {
  /// Reorders the collection and returns an index `p` such that every element
  /// in `range.lowerBound..<p` is less than every element in
  /// `p..<range.upperBound`.
  ///
  /// - Precondition: The count of `range` must be >= 3 i.e.
  ///   `distance(from: range.lowerBound, to: range.upperBound) >= 3`
  @inlinable
  internal mutating func _partition(
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows -> Index {
    var lo = range.lowerBound
    var hi = index(before: range.upperBound)

    // Sort the first, middle, and last elements, then use the middle value
    // as the pivot for the partition.
    let half = distance(from: lo, to: hi) / 2
    let mid = index(lo, offsetBy: half)
    try _sort3(lo, mid, hi, by: areInIncreasingOrder)
    
    // FIXME: Stashing the pivot element instead of using the index won't work
    // for move-only types.
    let pivot = self[mid]

    // Loop invariants:
    // * lo < hi
    // * self[i] < pivot, for i in range.lowerBound..<lo
    // * pivot <= self[i] for i in hi..<range.upperBound
    Loop: while true {
      FindLo: do {
        formIndex(after: &lo)
        while lo != hi {
          if try !areInIncreasingOrder(self[lo], pivot) { break FindLo }
          formIndex(after: &lo)
        }
        break Loop
      }

      FindHi: do {
        formIndex(before: &hi)
        while hi != lo {
          if try areInIncreasingOrder(self[hi], pivot) { break FindHi }
          formIndex(before: &hi)
        }
        break Loop
      }

      swapAt(lo, hi)
    }

    return lo
  }

  @inlinable
  public // @testable
  mutating func _introSort(
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {

    let n = distance(from: range.lowerBound, to: range.upperBound)
    guard n > 1 else { return }

    // Set max recursion depth to 2*floor(log(N)), as suggested in the introsort
    // paper: http://www.cs.rpi.edu/~musser/gp/introsort.ps
    let depthLimit = 2 * n._binaryLogarithm()
    try _introSortImpl(
      within: range,
      by: areInIncreasingOrder,
      depthLimit: depthLimit)
  }

  @inlinable
  internal mutating func _introSortImpl(
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool,
    depthLimit: Int
  ) rethrows {

    // Insertion sort is better at handling smaller regions.
    if distance(from: range.lowerBound, to: range.upperBound) < 20 {
      try _insertionSort(within: range, by: areInIncreasingOrder)
    } else if depthLimit == 0 {
      try _heapSort(within: range, by: areInIncreasingOrder)
    } else {
      // Partition and sort.
      // We don't check the depthLimit variable for underflow because this
      // variable is always greater than zero (see check above).
      let partIdx = try _partition(within: range, by: areInIncreasingOrder)
      try _introSortImpl(
        within: range.lowerBound..<partIdx,
        by: areInIncreasingOrder, 
        depthLimit: depthLimit &- 1)
      try _introSortImpl(
        within: partIdx..<range.upperBound,
        by: areInIncreasingOrder, 
        depthLimit: depthLimit &- 1)      
    }
  }

  @inlinable
  internal mutating func _siftDown(
    _ idx: Index,
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    var idx = idx
    var countToIndex = distance(from: range.lowerBound, to: idx)
    var countFromIndex = distance(from: idx, to: range.upperBound)
    // Check if left child is within bounds. If not, stop iterating, because
    // there are no children of the given node in the heap.
    while countToIndex + 1 < countFromIndex {
      let left = index(idx, offsetBy: countToIndex + 1)
      var largest = idx
      if try areInIncreasingOrder(self[largest], self[left]) {
        largest = left
      }
      // Check if right child is also within bounds before trying to examine it.
      if countToIndex + 2 < countFromIndex {
        let right = index(after: left)
        if try areInIncreasingOrder(self[largest], self[right]) {
          largest = right
        }
      }
      // If a child is bigger than the current node, swap them and continue 
      // sifting down.
      if largest != idx {
        swapAt(idx, largest)
        idx = largest
        countToIndex = distance(from: range.lowerBound, to: idx)
        countFromIndex = distance(from: idx, to: range.upperBound)
      } else {
        break
      }
    }
  }

  @inlinable
  internal mutating func _heapify(
    within range: Range<Index>, 
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    // Here we build a heap starting from the lowest nodes and moving to the
    // root. On every step we sift down the current node to obey the max-heap
    // property:
    //   parent >= max(leftChild, rightChild)
    //
    // We skip the rightmost half of the array, because these nodes don't have
    // any children.
    let root = range.lowerBound
    let half = distance(from: range.lowerBound, to: range.upperBound) / 2
    var node = index(root, offsetBy: half)

    while node != root {
      formIndex(before: &node)
      try _siftDown(node, within: range, by: areInIncreasingOrder)
    }
  }

  @inlinable
  public // @testable
  mutating func _heapSort(
    within range: Range<Index>,
    by areInIncreasingOrder: (Element, Element) throws -> Bool
  ) rethrows {
    var hi = range.upperBound
    let lo = range.lowerBound
    try _heapify(within: range, by: areInIncreasingOrder)
    formIndex(before: &hi)
    while hi != lo {
      swapAt(lo, hi)
      try _siftDown(lo, within: lo..<hi, by: areInIncreasingOrder)
      formIndex(before: &hi)
    }
  }
}
