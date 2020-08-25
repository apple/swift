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

extension String {
  // FIXME(strings): at least temporarily remove it to see where it was applied
  /// Creates a new string from the given substring.
  ///
  /// - Parameter substring: A substring to convert to a standalone `String`
  ///   instance.
  ///
  /// - Complexity: O(*n*), where *n* is the length of `substring`.
  @inlinable
  public init(_ substring: __shared Substring) {
    self = String._fromSubstring(substring)
  }
}

/// A slice of a string.
///
/// When you create a slice of a string, a `Substring` instance is the result.
/// Operating on substrings is fast and efficient because a substring shares
/// its storage with the original string. The `Substring` type presents the
/// same interface as `String`, so you can avoid or defer any copying of the
/// string's contents.
///
/// The following example creates a `greeting` string, and then finds the
/// substring of the first sentence:
///
///     let greeting = "Hi there! It's nice to meet you! 👋"
///     let endOfSentence = greeting.firstIndex(of: "!")!
///     let firstSentence = greeting[...endOfSentence]
///     // firstSentence == "Hi there!"
///
/// You can perform many string operations on a substring. Here, we find the
/// length of the first sentence and create an uppercase version.
///
///     print("'\(firstSentence)' is \(firstSentence.count) characters long.")
///     // Prints "'Hi there!' is 9 characters long."
///
///     let shoutingSentence = firstSentence.uppercased()
///     // shoutingSentence == "HI THERE!"
///
/// Converting a Substring to a String
/// ==================================
///
/// This example defines a `rawData` string with some unstructured data, and
/// then uses the string's `prefix(while:)` method to create a substring of
/// the numeric prefix:
///
///     let rawInput = "126 a.b 22219 zzzzzz"
///     let numericPrefix = rawInput.prefix(while: { "0"..."9" ~= $0 })
///     // numericPrefix is the substring "126"
///
/// When you need to store a substring or pass it to a function that requires a
/// `String` instance, you can convert it to a `String` by using the
/// `String(_:)` initializer. Calling this initializer copies the contents of
/// the substring to a new string.
///
///     func parseAndAddOne(_ s: String) -> Int {
///         return Int(s, radix: 10)! + 1
///     }
///     _ = parseAndAddOne(numericPrefix)
///     // error: cannot convert value...
///     let incrementedPrefix = parseAndAddOne(String(numericPrefix))
///     // incrementedPrefix == 127
///
/// Alternatively, you can convert the function that takes a `String` to one
/// that is generic over the `StringProtocol` protocol. The following code
/// declares a generic version of the `parseAndAddOne(_:)` function:
///
///     func genericParseAndAddOne<S: StringProtocol>(_ s: S) -> Int {
///         return Int(s, radix: 10)! + 1
///     }
///     let genericallyIncremented = genericParseAndAddOne(numericPrefix)
///     // genericallyIncremented == 127
///
/// You can call this generic function with an instance of either `String` or
/// `Substring`.
///
/// - Important: Don't store substrings longer than you need them to perform a
///   specific operation. A substring holds a reference to the entire storage
///   of the string it comes from, not just to the portion it presents, even
///   when there is no other reference to the original string. Storing
///   substrings may, therefore, prolong the lifetime of string data that is
///   no longer otherwise accessible, which can appear to be memory leakage.
@frozen
public struct Substring {
  @usableFromInline
  internal var _slice: Slice<String>

  @inlinable
  internal init(_ slice: Slice<String>) {
    let _guts = slice.base._guts
    let start = _guts.scalarAlign(slice.startIndex)
    let end = _guts.scalarAlign(slice.endIndex)

    self._slice = Slice(
      base: slice.base,
      bounds: Range(uncheckedBounds: (start, end)))
    _invariantCheck()
  }

  @inline(__always)
  internal init(_ slice: _StringGutsSlice) {
    self.init(String(slice._guts)[slice.range])
  }

  /// Creates an empty substring.
  @inlinable @inline(__always)
  public init() {
    self.init(Slice())
  }
}

extension Substring {
  /// Returns the underlying string from which this Substring was derived.
  @_alwaysEmitIntoClient
  public var base: String { return _slice.base }

  @inlinable @inline(__always)
  internal var _wholeGuts: _StringGuts { return base._guts }

  @inlinable @inline(__always)
  internal var _offsetRange: Range<Int> {
    return Range(
      uncheckedBounds: (startIndex._encodedOffset, endIndex._encodedOffset))
  }

  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    // Indices are always scalar aligned
    _internalInvariant(
      _slice.startIndex == base._guts.scalarAlign(_slice.startIndex) &&
      _slice.endIndex == base._guts.scalarAlign(_slice.endIndex))

    self.base._invariantCheck()
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension Substring: StringProtocol {
  public typealias Index = String.Index
  public typealias SubSequence = Substring

  @inlinable @inline(__always)
  public var startIndex: Index { return _slice.startIndex }

  @inlinable @inline(__always)
  public var endIndex: Index { return _slice.endIndex }

  @inlinable @inline(__always)
  public func index(after i: Index) -> Index {
    _precondition(i < endIndex, "Cannot increment beyond endIndex")
    _precondition(i >= startIndex, "Cannot increment an invalid index")
    return _slice.index(after: i)
  }

  @inlinable @inline(__always)
  public func index(before i: Index) -> Index {
    _precondition(i <= endIndex, "Cannot decrement an invalid index")
    _precondition(i > startIndex, "Cannot decrement beyond startIndex")
    return _slice.index(before: i)
  }

  @inlinable @inline(__always)
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    let result = _slice.index(i, offsetBy: n)
    _precondition(
      (_slice._startIndex ... _slice.endIndex).contains(result),
      "Operation results in an invalid index")
    return result
  }

  @inlinable @inline(__always)
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    let result = _slice.index(i, offsetBy: n, limitedBy: limit)
    _precondition(result.map {
        (_slice._startIndex ... _slice.endIndex).contains($0)
      } ?? true,
      "Operation results in an invalid index")
    return result
  }

  @inlinable @inline(__always)
  public func distance(from start: Index, to end: Index) -> Int {
    return _slice.distance(from: start, to: end)
  }

  public subscript(i: Index) -> Character {
    get { return _slice[i] }
  }

  public mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) where C: Collection, C.Iterator.Element == Iterator.Element {
    _slice.replaceSubrange(bounds, with: newElements)
  }

  public mutating func replaceSubrange(
    _ bounds: Range<Index>, with newElements: Substring
  ) {
    replaceSubrange(bounds, with: newElements._slice)
  }

  /// Creates a string from the given Unicode code units in the specified
  /// encoding.
  ///
  /// - Parameters:
  ///   - codeUnits: A collection of code units encoded in the encoding
  ///     specified in `sourceEncoding`.
  ///   - sourceEncoding: The encoding in which `codeUnits` should be
  ///     interpreted.
  @inlinable // specialization
  public init<C: Collection, Encoding: _UnicodeEncoding>(
    decoding codeUnits: C, as sourceEncoding: Encoding.Type
  ) where C.Iterator.Element == Encoding.CodeUnit {
    self.init(String(decoding: codeUnits, as: sourceEncoding))
  }

  /// Creates a string from the null-terminated, UTF-8 encoded sequence of
  /// bytes at the given pointer.
  ///
  /// - Parameter nullTerminatedUTF8: A pointer to a sequence of contiguous,
  ///   UTF-8 encoded bytes ending just before the first zero byte.
  public init(cString nullTerminatedUTF8: UnsafePointer<CChar>) {
    self.init(String(cString: nullTerminatedUTF8))
  }

  /// Creates a string from the null-terminated sequence of bytes at the given
  /// pointer.
  ///
  /// - Parameters:
  ///   - nullTerminatedCodeUnits: A pointer to a sequence of contiguous code
  ///     units in the encoding specified in `sourceEncoding`, ending just
  ///     before the first zero code unit.
  ///   - sourceEncoding: The encoding in which the code units should be
  ///     interpreted.
  @inlinable // specialization
  public init<Encoding: _UnicodeEncoding>(
    decodingCString nullTerminatedCodeUnits: UnsafePointer<Encoding.CodeUnit>,
    as sourceEncoding: Encoding.Type
  ) {
    self.init(
      String(decodingCString: nullTerminatedCodeUnits, as: sourceEncoding))
  }

  /// Calls the given closure with a pointer to the contents of the string,
  /// represented as a null-terminated sequence of UTF-8 code units.
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withCString(_:)`. Do not store or return the pointer for
  /// later use.
  ///
  /// - Parameter body: A closure with a pointer parameter that points to a
  ///   null-terminated sequence of UTF-8 code units. If `body` has a return
  ///   value, that value is also used as the return value for the
  ///   `withCString(_:)` method. The pointer argument is valid only for the
  ///   duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable // specialization
  public func withCString<Result>(
    _ body: (UnsafePointer<CChar>) throws -> Result) rethrows -> Result {
    // TODO(String performance): Detect when we cover the rest of a nul-
    // terminated String, and thus can avoid a copy.
    return try String(self).withCString(body)
  }

  /// Calls the given closure with a pointer to the contents of the string,
  /// represented as a null-terminated sequence of code units.
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withCString(encodedAs:_:)`. Do not store or return the
  /// pointer for later use.
  ///
  /// - Parameters:
  ///   - body: A closure with a pointer parameter that points to a
  ///     null-terminated sequence of code units. If `body` has a return
  ///     value, that value is also used as the return value for the
  ///     `withCString(encodedAs:_:)` method. The pointer argument is valid
  ///     only for the duration of the method's execution.
  ///   - targetEncoding: The encoding in which the code units should be
  ///     interpreted.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable // specialization
  public func withCString<Result, TargetEncoding: _UnicodeEncoding>(
    encodedAs targetEncoding: TargetEncoding.Type,
    _ body: (UnsafePointer<TargetEncoding.CodeUnit>) throws -> Result
  ) rethrows -> Result {
    // TODO(String performance): Detect when we cover the rest of a nul-
    // terminated String, and thus can avoid a copy.
    return try String(self).withCString(encodedAs: targetEncoding, body)
  }
}

#if SWIFT_ENABLE_REFLECTION
extension Substring: CustomReflectable {
 public var customMirror: Mirror { return String(self).customMirror }
}
#endif

extension Substring: CustomStringConvertible {
  @inlinable @inline(__always)
  public var description: String { return String(self) }
}

extension Substring: CustomDebugStringConvertible {
  public var debugDescription: String { return String(self).debugDescription }
}

extension Substring: LosslessStringConvertible {
  public init(_ content: String) {
    self = content[...]
  }
}

extension Substring {
  @frozen
  public struct UTF8View {
    @usableFromInline
    internal var _slice: Slice<String.UTF8View>
  }
}

extension Substring.UTF8View: BidirectionalCollection {
  public typealias Index = String.UTF8View.Index
  public typealias Indices = String.UTF8View.Indices
  public typealias Element = String.UTF8View.Element
  public typealias SubSequence = Substring.UTF8View

  /// Creates an instance that slices `base` at `_bounds`.
  @inlinable
  internal init(_ base: String.UTF8View, _bounds: Range<Index>) {
    _slice = Slice(
      base: String(base._guts).utf8,
      bounds: _bounds)
  }

  //
  // Plumb slice operations through
  //
  @inlinable
  public var startIndex: Index { return _slice.startIndex }

  @inlinable
  public var endIndex: Index { return _slice.endIndex }

  @inlinable
  public subscript(index: Index) -> Element { return _slice[index] }

  @inlinable
  public var indices: Indices { return _slice.indices }

  @inlinable
  public func index(after i: Index) -> Index { return _slice.index(after: i) }

  @inlinable
  public func formIndex(after i: inout Index) {
    _slice.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    return _slice.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    return _slice.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return _slice.distance(from: start, to: end)
  }

  @_alwaysEmitIntoClient
  @inlinable
  public func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R? {
    return try _slice.withContiguousStorageIfAvailable(body)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    _slice._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    _slice._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index { return _slice.index(before: i) }

  @inlinable
  public func formIndex(before i: inout Index) {
    _slice.formIndex(before: &i)
  }

  @inlinable
  public subscript(r: Range<Index>) -> Substring.UTF8View {
    // FIXME(strings): tests.
    _precondition(r.lowerBound >= startIndex && r.upperBound <= endIndex,
      "UTF8View index range out of bounds")
    return Substring.UTF8View(_slice.base, _bounds: r)
  }
}

extension Substring {
  @inlinable
  public var utf8: UTF8View {
    get {
      return base.utf8[startIndex..<endIndex]
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UTF8View) {
    self = String(
      content._slice.base._guts
    )[content.startIndex..<content.endIndex]
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// If `codeUnits` is an ill-formed code unit sequence, the result is `nil`.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-16.
  public init?(_ codeUnits: Substring.UTF8View) {
    let guts = codeUnits._slice.base._guts
    guard guts.isOnUnicodeScalarBoundary(codeUnits._slice.startIndex),
          guts.isOnUnicodeScalarBoundary(codeUnits._slice.endIndex) else {
      return nil
    }

    self = String(Substring(codeUnits))
  }
}
extension Substring {
  @frozen
  public struct UTF16View {
    @usableFromInline
    internal var _slice: Slice<String.UTF16View>
  }
}

extension Substring.UTF16View: BidirectionalCollection {
  public typealias Index = String.UTF16View.Index
  public typealias Indices = String.UTF16View.Indices
  public typealias Element = String.UTF16View.Element
  public typealias SubSequence = Substring.UTF16View

  /// Creates an instance that slices `base` at `_bounds`.
  @inlinable
  internal init(_ base: String.UTF16View, _bounds: Range<Index>) {
    _slice = Slice(
      base: String(base._guts).utf16,
      bounds: _bounds)
  }

  //
  // Plumb slice operations through
  //
  @inlinable
  public var startIndex: Index { return _slice.startIndex }

  @inlinable
  public var endIndex: Index { return _slice.endIndex }

  @inlinable
  public subscript(index: Index) -> Element { return _slice[index] }

  @inlinable
  public var indices: Indices { return _slice.indices }

  @inlinable
  public func index(after i: Index) -> Index { return _slice.index(after: i) }

  @inlinable
  public func formIndex(after i: inout Index) {
    _slice.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    return _slice.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    return _slice.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return _slice.distance(from: start, to: end)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    _slice._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    _slice._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index { return _slice.index(before: i) }

  @inlinable
  public func formIndex(before i: inout Index) {
    _slice.formIndex(before: &i)
  }

  @inlinable
  public subscript(r: Range<Index>) -> Substring.UTF16View {
    return Substring.UTF16View(_slice.base, _bounds: r)
  }
}

extension Substring {
  @inlinable
  public var utf16: UTF16View {
    get {
      return base.utf16[startIndex..<endIndex]
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UTF16View) {
    self = String(
      content._slice.base._guts
    )[content.startIndex..<content.endIndex]
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// If `codeUnits` is an ill-formed code unit sequence, the result is `nil`.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-16.
  public init?(_ codeUnits: Substring.UTF16View) {
    let guts = codeUnits._slice.base._guts
    guard guts.isOnUnicodeScalarBoundary(codeUnits._slice.startIndex),
          guts.isOnUnicodeScalarBoundary(codeUnits._slice.endIndex) else {
      return nil
    }

    self = String(Substring(codeUnits))
  }
}
extension Substring {
  @frozen
  public struct UnicodeScalarView {
    @usableFromInline
    internal var _slice: Slice<String.UnicodeScalarView>
  }
}

extension Substring.UnicodeScalarView: BidirectionalCollection {
  public typealias Index = String.UnicodeScalarView.Index
  public typealias Indices = String.UnicodeScalarView.Indices
  public typealias Element = String.UnicodeScalarView.Element
  public typealias SubSequence = Substring.UnicodeScalarView

  /// Creates an instance that slices `base` at `_bounds`.
  @inlinable
  internal init(_ base: String.UnicodeScalarView, _bounds: Range<Index>) {
    _slice = Slice(
      base: String(base._guts).unicodeScalars,
      bounds: _bounds)
  }

  //
  // Plumb slice operations through
  //
  @inlinable
  public var startIndex: Index { return _slice.startIndex }

  @inlinable
  public var endIndex: Index { return _slice.endIndex }

  @inlinable
  public subscript(index: Index) -> Element { return _slice[index] }

  @inlinable
  public var indices: Indices { return _slice.indices }

  @inlinable
  public func index(after i: Index) -> Index { return _slice.index(after: i) }

  @inlinable
  public func formIndex(after i: inout Index) {
    _slice.formIndex(after: &i)
  }

  @inlinable
  public func index(_ i: Index, offsetBy n: Int) -> Index {
    return _slice.index(i, offsetBy: n)
  }

  @inlinable
  public func index(
    _ i: Index, offsetBy n: Int, limitedBy limit: Index
  ) -> Index? {
    return _slice.index(i, offsetBy: n, limitedBy: limit)
  }

  @inlinable
  public func distance(from start: Index, to end: Index) -> Int {
    return _slice.distance(from: start, to: end)
  }

  @inlinable
  public func _failEarlyRangeCheck(_ index: Index, bounds: Range<Index>) {
    _slice._failEarlyRangeCheck(index, bounds: bounds)
  }

  @inlinable
  public func _failEarlyRangeCheck(
    _ range: Range<Index>, bounds: Range<Index>
  ) {
    _slice._failEarlyRangeCheck(range, bounds: bounds)
  }

  @inlinable
  public func index(before i: Index) -> Index { return _slice.index(before: i) }

  @inlinable
  public func formIndex(before i: inout Index) {
    _slice.formIndex(before: &i)
  }

  @inlinable
  public subscript(r: Range<Index>) -> Substring.UnicodeScalarView {
    return Substring.UnicodeScalarView(_slice.base, _bounds: r)
  }
}

extension Substring {
  @inlinable
  public var unicodeScalars: UnicodeScalarView {
    get {
      return base.unicodeScalars[startIndex..<endIndex]
    }
    set {
      self = Substring(newValue)
    }
  }

  /// Creates a Substring having the given content.
  ///
  /// - Complexity: O(1)
  public init(_ content: UnicodeScalarView) {
    self = String(
      content._slice.base._guts
    )[content.startIndex..<content.endIndex]
  }
}

extension String {
  /// Creates a String having the given content.
  ///
  /// - Complexity: O(N), where N is the length of the resulting `String`'s
  ///   UTF-16.
  public init(_ content: Substring.UnicodeScalarView) {
    self = String(Substring(content))
  }
}

// FIXME: The other String views should be RangeReplaceable too.
extension Substring.UnicodeScalarView: RangeReplaceableCollection {
  @inlinable
  public init() { _slice = Slice.init() }

  public mutating func replaceSubrange<C: Collection>(
    _ target: Range<Index>, with replacement: C
  ) where C.Element == Element {
    _slice.replaceSubrange(target, with: replacement)
  }
}

extension Substring: RangeReplaceableCollection {
  @_specialize(where S == String)
  @_specialize(where S == Substring)
  @_specialize(where S == Array<Character>)
  public init<S: Sequence>(_ elements: S)
  where S.Element == Character {
    if let str = elements as? String {
      self = str[...]
      return
    }
    if let subStr = elements as? Substring {
      self = subStr
      return
    }
    self = String(elements)[...]
  }

  @inlinable // specialize
  public mutating func append<S: Sequence>(contentsOf elements: S)
  where S.Element == Character {
    var string = String(self)
    self = Substring() // Keep unique storage if possible
    string.append(contentsOf: elements)
    self = Substring(string)
  }
}

extension Substring {
  public func lowercased() -> String {
    return String(self).lowercased()
  }

  public func uppercased() -> String {
    return String(self).uppercased()
  }

  public func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> String {
    return try String(self.lazy.filter(isIncluded))
  }
}

extension Substring: TextOutputStream {
  public mutating func write(_ other: String) {
    append(contentsOf: other)
  }
}

extension Substring: TextOutputStreamable {
  @inlinable // specializable
  public func write<Target: TextOutputStream>(to target: inout Target) {
    target.write(String(self))
  }
}

extension Substring: ExpressibleByUnicodeScalarLiteral {
  @inlinable
  public init(unicodeScalarLiteral value: String) {
     self.init(value)
  }
}
extension Substring: ExpressibleByExtendedGraphemeClusterLiteral {
  @inlinable
  public init(extendedGraphemeClusterLiteral value: String) {
     self.init(value)
  }
}

extension Substring: ExpressibleByStringLiteral {
  @inlinable
  public init(stringLiteral value: String) {
     self.init(value)
  }
}

// String/Substring Slicing
extension String {
  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> Substring {
    _boundsCheck(r)
    return Substring(Slice(base: self, bounds: r))
  }
}

extension Substring {
  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> Substring {
    return Substring(_slice[r])
  }
}
