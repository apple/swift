//===--- StdlibCoreExtras.swift -------------------------------------------===//
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

import SwiftPrivate
import SwiftPrivateLibcExtras
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(WASI)
import WASILibc
#elseif os(Windows)
import CRT
#endif

#if _runtime(_ObjC)
import Foundation
#endif

//
// These APIs don't really belong in a unit testing library, but they are
// useful in tests, and stdlib does not have such facilities yet.
//

func findSubstring(_ haystack: Substring, _ needle: String) -> String.Index? {
  return findSubstring(haystack._ephemeralString, needle)
}

func findSubstring(_ string: String, _ substring: String) -> String.Index? {
  if substring.isEmpty {
    return string.startIndex
  }
#if _runtime(_ObjC)
  return string.range(of: substring)?.lowerBound
#else
  // FIXME(performance): This is a very non-optimal algorithm, with a worst
  // case of O((n-m)*m). When non-objc String has a match function that's better,
  // this should be removed in favor of using that.

  // Operate on unicode scalars rather than codeunits.
  let haystack = string.unicodeScalars
  let needle = substring.unicodeScalars

  for matchStartIndex in haystack.indices {
    var matchIndex = matchStartIndex
    var needleIndex = needle.startIndex
    while true {
      if needleIndex == needle.endIndex {
        // if we hit the end of the search string, we found the needle
        return matchStartIndex
      }
      if matchIndex == haystack.endIndex {
        // if we hit the end of the string before finding the end of the needle,
        // we aren't going to find the needle after that.
        return nil
      }
      if needle[needleIndex] == haystack[matchIndex] {
        // keep advancing through both the string and search string on match
        matchIndex = haystack.index(after: matchIndex)
        needleIndex = haystack.index(after: needleIndex)
      } else {
        // no match, go back to finding a starting match in the string.
        break
      }
    }
  }
  return nil
#endif
}

#if !os(Windows)
public func createTemporaryFile(
  _ fileNamePrefix: String, _ fileNameSuffix: String, _ contents: String
) -> String {
#if _runtime(_ObjC)
  let tempDir: NSString = NSTemporaryDirectory() as NSString
  var fileName = tempDir.appendingPathComponent(
    fileNamePrefix + "XXXXXX" + fileNameSuffix)
#else
  var fileName = fileNamePrefix + "XXXXXX" + fileNameSuffix
#endif
  let fd = _stdlib_mkstemps(
    &fileName, CInt(fileNameSuffix.utf8.count))
  if fd < 0 {
    fatalError("mkstemps() returned an error")
  }
  var stream = _FDOutputStream(fd: fd)
  stream.write(contents)
  if close(fd) != 0 {
    fatalError("close() return an error")
  }
  return fileName
}
#endif

public final class Box<T> {
  public init(_ value: T) { self.value = value }
  public var value: T
}

infix operator <=>

public func <=> <T: Comparable>(lhs: T, rhs: T) -> ExpectedComparisonResult {
  return lhs < rhs
    ? .lt
    : lhs > rhs ? .gt : .eq
}

public struct TypeIdentifier : Hashable, Comparable {
  public var value: Any.Type

  public init(_ value: Any.Type) {
    self.value = value
  }

  public var hashValue: Int { return objectID.hashValue }
  public func hash(into hasher: inout Hasher) {
    hasher.combine(objectID)
  }

  internal var objectID : ObjectIdentifier {
    return ObjectIdentifier(value)
  }

  public static func < (lhs: TypeIdentifier, rhs: TypeIdentifier) -> Bool {
    return lhs.objectID < rhs.objectID
  }

  public static func == (lhs: TypeIdentifier, rhs: TypeIdentifier) -> Bool {
    return lhs.objectID == rhs.objectID
  }
}

extension TypeIdentifier
  : CustomStringConvertible, CustomDebugStringConvertible {
  public var description: String {
    return String(describing: value)
  }
  public var debugDescription: String {
    return "TypeIdentifier(\(description))"
  }
}

enum FormNextPermutationResult {
  case success
  case formedFirstPermutation
}

extension MutableCollection
  where
  Self : BidirectionalCollection,
  Iterator.Element : Comparable
{
  mutating func _reverseSubrange(_ subrange: Range<Index>) {
    if subrange.isEmpty { return }
    var f = subrange.lowerBound
    var l = index(before: subrange.upperBound)
    while f < l {
      swapAt(f, l)
      formIndex(after: &f)
      formIndex(before: &l)
    }
  }

  mutating func formNextPermutation() -> FormNextPermutationResult {
    if isEmpty {
      // There are 0 elements, only one permutation is possible.
      return .formedFirstPermutation
    }

    do {
      var i = startIndex
      formIndex(after: &i)
      if i == endIndex {
        // There is only element, only one permutation is possible.
        return .formedFirstPermutation
      }
    }

    var i = endIndex
    formIndex(before: &i)
    var beforeI = i
    formIndex(before: &beforeI)
    var elementAtI = self[i]
    var elementAtBeforeI = self[beforeI]
    while true {
      if elementAtBeforeI < elementAtI {
        // Elements at `i..<endIndex` are in non-increasing order.  To form the
        // next permutation in lexicographical order we need to replace
        // `self[i-1]` with the next larger element found in the tail, and
        // reverse the tail.  For example:
        //
        //       i-1 i        endIndex
        //        V  V           V
        //     6  2  8  7  4  1 [ ]  // Input.
        //     6 (4) 8  7 (2) 1 [ ]  // Exchanged self[i-1] with the
        //        ^--------^         // next larger element
        //                           // from the tail.
        //     6  4 (1)(2)(7)(8)[ ]  // Reversed the tail.
        //           <-------->
        var j = endIndex
        repeat {
          formIndex(before: &j)
        } while !(elementAtBeforeI < self[j])
        swapAt(beforeI, j)
        _reverseSubrange(i..<endIndex)
        return .success
      }
      if beforeI == startIndex {
        // All elements are in non-increasing order.  Reverse to form the first
        // permutation, where all elements are sorted (in non-increasing order).
        reverse()
        return .formedFirstPermutation
      }
      i = beforeI
      formIndex(before: &beforeI)
      elementAtI = elementAtBeforeI
      elementAtBeforeI = self[beforeI]
    }
  }
}

/// Generate all permutations.
public func forAllPermutations(_ size: Int, _ body: ([Int]) -> Void) {
  var data = Array(0..<size)
  repeat {
    body(data)
  } while data.formNextPermutation() != .formedFirstPermutation
}

/// Generate all permutations.
public func forAllPermutations<S : Sequence>(
  _ sequence: S, _ body: ([S.Element]) -> Void
) {
  let data = Array(sequence)
  forAllPermutations(data.count) {
    (indices: [Int]) in
    body(indices.map { data[$0] })
    return ()
  }
}

public func cartesianProduct<C1 : Collection, C2 : Collection>(
  _ c1: C1, _ c2: C2
) -> [(C1.Element, C2.Element)] {
  var result: [(C1.Element, C2.Element)] = []
  for e1 in c1 {
    for e2 in c2 {
      result.append((e1, e2))
    }
  }
  return result
}

/// Return true if the standard library was compiled in a debug configuration.
public func _isStdlibDebugConfiguration() -> Bool {
#if SWIFT_STDLIB_DEBUG
  return true
#else
  return false
#endif
}

// Return true if the Swift runtime available is at least 5.1
public func _hasSwift_5_1() -> Bool {
  if #available(SwiftStdlib 5.1, *) {
    return true
  }
  return false
}

@frozen
public struct LinearCongruentialGenerator: RandomNumberGenerator {

  @usableFromInline
  internal var _state: UInt64

  @inlinable
  public init(seed: UInt64) {
    _state = seed
    for _ in 0 ..< 10 { _ = next() }
  }

  @inlinable
  public mutating func next() -> UInt64 {
    _state = 2862933555777941757 &* _state &+ 3037000493
    return _state
  }
}

#if !SWIFT_ENABLE_REFLECTION

public func dump<T, TargetStream: TextOutputStream>(_ value: T, to target: inout TargetStream) {
  target.write("(reflection not available)")
}

#endif

#if SWIFT_STDLIB_STATIC_PRINT

public func print(_ s: Any, terminator: String = "\n") {
  let data = Array("\(s)\(terminator)".utf8)
  write(STDOUT_FILENO, data, data.count)
}

public func print<Target>(_ s: Any, terminator: String = "\n", to output: inout Target) where Target : TextOutputStream {
  output.write("\(s)\(terminator)")
}

#endif
