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

// FIXME(ABI)#71 : The UTF-16 string view should have a custom iterator type to
// allow performance optimizations of linear traversals.

extension String {
  /// A view of a string's contents as a collection of Unicode scalar values.
  ///
  /// You can access a string's view of Unicode scalar values by using its
  /// `unicodeScalars` property. Unicode scalar values are the 21-bit codes
  /// that are the basic unit of Unicode. Each scalar value is represented by
  /// a `Unicode.Scalar` instance and is equivalent to a UTF-32 code unit.
  ///
  ///     let flowers = "Flowers 💐"
  ///     for v in flowers.unicodeScalars {
  ///         print(v.value)
  ///     }
  ///     // 70
  ///     // 108
  ///     // 111
  ///     // 119
  ///     // 101
  ///     // 114
  ///     // 115
  ///     // 32
  ///     // 128144
  ///
  /// Some characters that are visible in a string are made up of more than one
  /// Unicode scalar value. In that case, a string's `unicodeScalars` view
  /// contains more elements than the string itself.
  ///
  ///     let flag = "🇵🇷"
  ///     for c in flag {
  ///         print(c)
  ///     }
  ///     // 🇵🇷
  ///
  ///     for v in flag.unicodeScalars {
  ///         print(v.value)
  ///     }
  ///     // 127477
  ///     // 127479
  ///
  /// You can convert a `String.UnicodeScalarView` instance back into a string
  /// using the `String` type's `init(_:)` initializer.
  ///
  ///     let favemoji = "My favorite emoji is 🎉"
  ///     if let i = favemoji.unicodeScalars.firstIndex(where: { $0.value >= 128 }) {
  ///         let asciiPrefix = String(favemoji.unicodeScalars[..<i])
  ///         print(asciiPrefix)
  ///     }
  ///     // Prints "My favorite emoji is "
  @frozen
  public struct UnicodeScalarView {
    @usableFromInline
    internal var _guts: _StringGuts

    @inlinable @inline(__always)
    internal init(_ _guts: _StringGuts) {
      self._guts = _guts
      _invariantCheck()
    }
  }
}

extension String.UnicodeScalarView {
  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
    // TODO: Assert start/end are scalar aligned
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

extension String.UnicodeScalarView: BidirectionalCollection {
  public typealias Index = String.Index

  /// The position of the first Unicode scalar value if the string is
  /// nonempty.
  ///
  /// If the string is empty, `startIndex` is equal to `endIndex`.
  @inlinable @inline(__always)
  public var startIndex: Index { return _guts.startIndex }

  /// The "past the end" position---that is, the position one greater than
  /// the last valid subscript argument.
  ///
  /// In an empty Unicode scalars view, `endIndex` is equal to `startIndex`.
  @inlinable @inline(__always)
  public var endIndex: Index { return _guts.endIndex }

  /// Returns the next consecutive location after `i`.
  ///
  /// - Precondition: The next location exists.
  @inlinable @inline(__always)
  public func index(after i: Index) -> Index {
    let i = _guts.scalarAlign(i)
    _internalInvariant(i < endIndex)
    // TODO(String performance): isASCII fast-path

    if _fastPath(_guts.isFastUTF8) {
      let len = _guts.fastUTF8ScalarLength(startingAt: i._encodedOffset)
      return i.encoded(offsetBy: len)._scalarAligned
    }

    return _foreignIndex(after: i)
  }

  @_alwaysEmitIntoClient // Swift 5.1 bug fix
  public func distance(from start: Index, to end: Index) -> Int {
    return _distance(from: _guts.scalarAlign(start), to: _guts.scalarAlign(end))
  }

  /// Returns the previous consecutive location before `i`.
  ///
  /// - Precondition: The previous location exists.
  @inlinable @inline(__always)
  public func index(before i: Index) -> Index {
    let i = _guts.scalarAlign(i)
    precondition(i._encodedOffset > 0)
    // TODO(String performance): isASCII fast-path

    if _fastPath(_guts.isFastUTF8) {
      let len = _guts.withFastUTF8 { utf8 -> Int in
        return _utf8ScalarLength(utf8, endingAt: i._encodedOffset)
      }
      _internalInvariant(len <= 4, "invalid UTF8")
      return i.encoded(offsetBy: -len)._scalarAligned
    }

    return _foreignIndex(before: i)
  }

  /// Accesses the Unicode scalar value at the given position.
  ///
  /// The following example searches a string's Unicode scalars view for a
  /// capital letter and then prints the character and Unicode scalar value
  /// at the found index:
  ///
  ///     let greeting = "Hello, friend!"
  ///     if let i = greeting.unicodeScalars.firstIndex(where: { "A"..."Z" ~= $0 }) {
  ///         print("First capital letter: \(greeting.unicodeScalars[i])")
  ///         print("Unicode scalar value: \(greeting.unicodeScalars[i].value)")
  ///     }
  ///     // Prints "First capital letter: H"
  ///     // Prints "Unicode scalar value: 72"
  ///
  /// - Parameter position: A valid index of the character view. `position`
  ///   must be less than the view's end index.
  @inlinable @inline(__always)
  public subscript(position: Index) -> Unicode.Scalar {
    String(_guts)._boundsCheck(position)
    let i = _guts.scalarAlign(position)
    return _guts.errorCorrectedScalar(startingAt: i._encodedOffset).0
  }
}

extension String.UnicodeScalarView {
  @frozen
  public struct Iterator: IteratorProtocol {
    @usableFromInline
    internal var _guts: _StringGuts

    @usableFromInline
    internal var _position: Int = 0

    @usableFromInline
    internal var _end: Int

    @inlinable
    internal init(_ guts: _StringGuts) {
      self._end = guts.count
      self._guts = guts
    }

    @inlinable
    @inline(__always)
    public mutating func next() -> Unicode.Scalar? {
      guard _fastPath(_position < _end) else { return nil }

      let (result, len) = _guts.errorCorrectedScalar(startingAt: _position)
      _position &+= len
      return result
    }
  }
  @inlinable
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_guts)
  }
}

extension String.UnicodeScalarView: CustomStringConvertible {
 @inlinable @inline(__always)
 public var description: String { return String(_guts) }
}

extension String.UnicodeScalarView: CustomDebugStringConvertible {
 public var debugDescription: String {
   return "StringUnicodeScalarView(\(self.description.debugDescription))"
 }
}

extension String {
  /// Creates a string corresponding to the given collection of Unicode
  /// scalars.
  ///
  /// You can use this initializer to create a new string from a slice of
  /// another string's `unicodeScalars` view.
  ///
  ///     let picnicGuest = "Deserving porcupine"
  ///     if let i = picnicGuest.unicodeScalars.firstIndex(of: " ") {
  ///         let adjective = String(picnicGuest.unicodeScalars[..<i])
  ///         print(adjective)
  ///     }
  ///     // Prints "Deserving"
  ///
  /// The `adjective` constant is created by calling this initializer with a
  /// slice of the `picnicGuest.unicodeScalars` view.
  ///
  /// - Parameter unicodeScalars: A collection of Unicode scalar values.
  @inlinable @inline(__always)
  public init(_ unicodeScalars: UnicodeScalarView) {
    self.init(unicodeScalars._guts)
  }

  /// The index type for a string's `unicodeScalars` view.
  public typealias UnicodeScalarIndex = UnicodeScalarView.Index

  /// The string's value represented as a collection of Unicode scalar values.
  @inlinable
  public var unicodeScalars: UnicodeScalarView {
    @inline(__always) get { return UnicodeScalarView(_guts) }
    @inline(__always) set { _guts = newValue._guts }
  }
}

extension String.UnicodeScalarView: RangeReplaceableCollection {
  /// Creates an empty view instance.
  @inlinable @inline(__always)
  public init() {
    self.init(_StringGuts())
  }

  /// Reserves enough space in the view's underlying storage to store the
  /// specified number of ASCII characters.
  ///
  /// Because a Unicode scalar value can require more than a single ASCII
  /// character's worth of storage, additional allocation may be necessary
  /// when adding to a Unicode scalar view after a call to
  /// `reserveCapacity(_:)`.
  ///
  /// - Parameter n: The minimum number of ASCII character's worth of storage
  ///   to allocate.
  ///
  /// - Complexity: O(*n*), where *n* is the capacity being reserved.
  public mutating func reserveCapacity(_ n: Int) {
    self._guts.reserveCapacity(n)
  }

  /// Appends the given Unicode scalar to the view.
  ///
  /// - Parameter c: The character to append to the string.
  public mutating func append(_ c: Unicode.Scalar) {
    self._guts.append(String(c)._guts)
  }

  /// Appends the Unicode scalar values in the given sequence to the view.
  ///
  /// - Parameter newElements: A sequence of Unicode scalar values.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the resulting view.
  public mutating func append<S: Sequence>(contentsOf newElements: S)
  where S.Element == Unicode.Scalar {
    // TODO(String performance): Skip extra String allocation
    let scalars = String(decoding: newElements.map { $0.value }, as: UTF32.self)
    self = (String(self._guts) + scalars).unicodeScalars
  }

  /// Replaces the elements within the specified bounds with the given Unicode
  /// scalar values.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameters:
  ///   - bounds: The range of elements to replace. The bounds of the range
  ///     must be valid indices of the view.
  ///   - newElements: The new Unicode scalar values to add to the string.
  ///
  /// - Complexity: O(*m*), where *m* is the combined length of the view and
  ///   `newElements`. If the call to `replaceSubrange(_:with:)` simply
  ///   removes elements at the end of the string, the complexity is O(*n*),
  ///   where *n* is equal to `bounds.count`.
  public mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) where C: Collection, C.Element == Unicode.Scalar {
    // TODO(String performance): Skip extra String and Array allocation

    let utf8Replacement = newElements.flatMap { String($0).utf8 }
    let replacement = utf8Replacement.withUnsafeBufferPointer {
      return String._uncheckedFromUTF8($0)
    }
    var copy = String(_guts)
    copy.replaceSubrange(bounds, with: replacement)
    self = copy.unicodeScalars
  }
}

// Index conversions
extension String.UnicodeScalarIndex {
  /// Creates an index in the given Unicode scalars view that corresponds
  /// exactly to the specified `UTF16View` position.
  ///
  /// The following example finds the position of a space in a string's `utf16`
  /// view and then converts that position to an index in the string's
  /// `unicodeScalars` view:
  ///
  ///     let cafe = "Café 🍵"
  ///
  ///     let utf16Index = cafe.utf16.firstIndex(of: 32)!
  ///     let scalarIndex = String.Index(utf16Index, within: cafe.unicodeScalars)!
  ///
  ///     print(String(cafe.unicodeScalars[..<scalarIndex]))
  ///     // Prints "Café"
  ///
  /// If the index passed as `sourcePosition` doesn't have an exact
  /// corresponding position in `unicodeScalars`, the result of the
  /// initializer is `nil`. For example, an attempt to convert the position of
  /// the trailing surrogate of a UTF-16 surrogate pair results in `nil`.
  ///
  /// - Parameters:
  ///   - sourcePosition: A position in the `utf16` view of a string.
  ///     `utf16Index` must be an element of
  ///     `String(unicodeScalars).utf16.indices`.
  ///   - unicodeScalars: The `UnicodeScalarView` in which to find the new
  ///     position.
  public init?(
    _ sourcePosition: String.Index,
    within unicodeScalars: String.UnicodeScalarView
  ) {
    guard unicodeScalars._guts.isOnUnicodeScalarBoundary(sourcePosition) else {
      return nil
    }
    self = sourcePosition
  }

  /// Returns the position in the given string that corresponds exactly to this
  /// index.
  ///
  /// This example first finds the position of a space (UTF-8 code point `32`)
  /// in a string's `utf8` view and then uses this method find the same position
  /// in the string.
  ///
  ///     let cafe = "Café 🍵"
  ///     let i = cafe.unicodeScalars.firstIndex(of: "🍵")!
  ///     let j = i.samePosition(in: cafe)!
  ///     print(cafe[j...])
  ///     // Prints "🍵"
  ///
  /// - Parameter characters: The string to use for the index conversion.
  ///   This index must be a valid index of at least one view of `characters`.
  /// - Returns: The position in `characters` that corresponds exactly to
  ///   this index. If this index does not have an exact corresponding
  ///   position in `characters`, this method returns `nil`. For example,
  ///   an attempt to convert the position of a UTF-8 continuation byte
  ///   returns `nil`.
  public func samePosition(in characters: String) -> String.Index? {
    return String.Index(self, within: characters)
  }
}

#if SWIFT_ENABLE_REFLECTION
// Reflection
extension String.UnicodeScalarView: CustomReflectable {
  /// Returns a mirror that reflects the Unicode scalars view of a string.
  public var customMirror: Mirror {
    return Mirror(self, unlabeledChildren: self)
  }
}
#endif

//===--- Slicing Support --------------------------------------------------===//
/// In Swift 3.2, in the absence of type context,
///
///   someString.unicodeScalars[
///     someString.unicodeScalars.startIndex
///     ..< someString.unicodeScalars.endIndex]
///
/// was deduced to be of type `String.UnicodeScalarView`.  Provide a
/// more-specific Swift-3-only `subscript` overload that continues to produce
/// `String.UnicodeScalarView`.
extension String.UnicodeScalarView {
  public typealias SubSequence = Substring.UnicodeScalarView

  @available(swift, introduced: 4)
  public subscript(r: Range<Index>) -> String.UnicodeScalarView.SubSequence {
    return String.UnicodeScalarView.SubSequence(self, _bounds: r)
  }
}

// Foreign string Support
extension String.UnicodeScalarView {
  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(after i: Index) -> Index {
    _internalInvariant(_guts.isForeign)
    let cu = _guts.foreignErrorCorrectedUTF16CodeUnit(at: i)
    let len = UTF16.isLeadSurrogate(cu) ? 2 : 1

    return i.encoded(offsetBy: len)._scalarAligned
  }

  @usableFromInline @inline(never)
  @_effects(releasenone)
  internal func _foreignIndex(before i: Index) -> Index {
    _internalInvariant(_guts.isForeign)
    let priorIdx = i.priorEncoded
    let cu = _guts.foreignErrorCorrectedUTF16CodeUnit(at: priorIdx)
    let len = UTF16.isTrailSurrogate(cu) ? 2 : 1

    return i.encoded(offsetBy: -len)._scalarAligned
  }
}
