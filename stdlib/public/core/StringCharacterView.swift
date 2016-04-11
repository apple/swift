//===--- StringCharacterView.swift - String's Collection of Characters ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  String is-not-a Sequence or Collection, but it exposes a
//  collection of characters.
//
//===----------------------------------------------------------------------===//

extension String {
  /// A `String`'s collection of `Character`s ([extended grapheme
  /// clusters](http://www.unicode.org/glossary/#extended_grapheme_cluster))
  /// elements.
  public struct CharacterView {
    internal var _coreOffset: Int
    internal var _core: _StringCore

    /// Create a view of the `Character`s in `text`.
    public init(_ text: String) {
      self._coreOffset = 0
      self._core = text._core
    }
    
    public // @testable
    init(_ _core: _StringCore) {
      self._coreOffset = 0
      self._core = _core
    }
    
    internal init(_ _core: _StringCore, offset: Int) {
      self._coreOffset = offset
      self._core = _core
    }
  }

  /// A collection of `Characters` representing the `String`'s
  /// [extended grapheme
  /// clusters](http://www.unicode.org/glossary/#extended_grapheme_cluster).
  public var characters: CharacterView {
    get {
      return CharacterView(self)
    }
    set {
      self = String(newValue)
    }
  }

  /// Efficiently mutate `self` by applying `body` to its `characters`.
  ///
  /// - Warning: Do not rely on anything about `self` (the `String`
  ///   that is the target of this method) during the execution of
  ///   `body`: it may not appear to have its correct value.  Instead,
  ///   use only the `String.CharacterView` argument to `body`.
  public mutating func withMutableCharacters<R>(body: (inout CharacterView) -> R) -> R {
    // Naively mutating self.characters forces multiple references to
    // exist at the point of mutation. Instead, temporarily move the
    // core of this string into a CharacterView.
    var tmp = CharacterView("")
    swap(&_core, &tmp._core)
    let r = body(&tmp)
    swap(&_core, &tmp._core)
    return r
  }

  /// Construct the `String` corresponding to the given sequence of
  /// Unicode scalars.
  public init(_ characters: CharacterView) {
    self.init(characters._core)
  }
}

/// `String.CharacterView` is a collection of `Character`.
extension String.CharacterView : BidirectionalCollection {
  internal typealias UnicodeScalarView = String.UnicodeScalarView
  internal var unicodeScalars: UnicodeScalarView {
    return UnicodeScalarView(_core, offset: _coreOffset)
  }
  
  /// A character position.
  public struct Index : Comparable, CustomPlaygroundQuickLookable {
    public // SPI(Foundation)    
    init(_base: UnicodeScalarView.Index, _characters: String.CharacterView) {
      self._base = _base
      self._countUTF16 =
        _characters._measureExtendedGraphemeClusterForward(from: _base)
    }

    internal init(_base: UnicodeScalarView.Index, _countUTF16: Int) {
      self._base = _base
      self._countUTF16 = _countUTF16
    }

    internal let _base: UnicodeScalarView.Index

    /// The count of this extended grapheme cluster in UTF-16 code units.
    internal let _countUTF16: Int

    /// The integer offset of this index in UTF-16 code units.
    public // SPI(Foundation)
    var _utf16Index: Int {
      return _base._position
    }

    /// The one past end index for this extended grapheme cluster in Unicode
    /// scalars.
    internal var _endBase: UnicodeScalarView.Index {
      return UnicodeScalarView.Index(
          _utf16Index + _countUTF16)
    }

    public var customPlaygroundQuickLook: PlaygroundQuickLook {
      return .int(Int64(_utf16Index))
    }
  }

  public typealias IndexDistance = Int

  /// The position of the first `Character` if `self` is
  /// non-empty; identical to `endIndex` otherwise.
  public var startIndex: Index {
    return Index(_base: unicodeScalars.startIndex, _characters: self)
  }

  /// The "past the end" position.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `successor(of:)`.
  public var endIndex: Index {
    return Index(_base: unicodeScalars.endIndex, _characters: self)
  }

  /// Returns the position of the next consecutive value after `i`.
  ///
  /// - Precondition: The next value is representable.
  // FIXME: swift-3-indexing-model: pull the following logic into UTF8View.successor(of: Index)
  @warn_unused_result
  public func successor(of i: Index) -> Index {
    // FIXME: swift-3-indexing-model: range check i?
    _precondition(i._base._position != _coreOffset + _core.count, "cannot increment endIndex")
    return Index(_base: i._endBase, _characters: self)
  }

  /// Returns the position of the previous consecutive value before `i`.
  ///
  /// - Precondition: The previous value is representable.
  // FIXME: swift-3-indexing-model: pull the following logic into UTF8View.predecessor(of: Index)
  @warn_unused_result
  public func predecessor(of i: Index) -> Index {
    _precondition(i._base._position != _coreOffset,
                  "cannot decrement startIndex")
    let predecessorLengthUTF16 =
      _measureExtendedGraphemeClusterBackward(from: i._base)
    return Index(
      _base: UnicodeScalarView.Index(
        i._utf16Index - predecessorLengthUTF16),
      _characters: self)
  }

  /// Returns the length of the first extended grapheme cluster in UTF-16
  /// code units.
  @warn_unused_result
  @inline(never)
  internal func _measureExtendedGraphemeClusterForward(
    from start: UnicodeScalarView.Index
    ) -> Int {
    var start = start
    var end = start
    end._position = _core.count + _coreOffset
    
    if start == end {
      return 0
    }
    
    let startIndexUTF16 = start._position
    let unicodeScalars = UnicodeScalarView(_core, offset: _coreOffset)
    let graphemeClusterBreakProperty =
      _UnicodeGraphemeClusterBreakPropertyTrie()
    let segmenter = _UnicodeExtendedGraphemeClusterSegmenter()
    
    var gcb0 = graphemeClusterBreakProperty.getPropertyRawValue(
      unicodeScalars[start].value)
    unicodeScalars.formSuccessor(&start)
    
    while start != end {
      // FIXME(performance): consider removing this "fast path".  A branch
      // that is hard to predict could be worse for performance than a few
      // loads from cache to fetch the property 'gcb1'.
      if segmenter.isBoundaryAfter(gcb0) {
        break
      }
      let gcb1 = graphemeClusterBreakProperty.getPropertyRawValue(
        unicodeScalars[start].value)
      if segmenter.isBoundary(gcb0, gcb1) {
        break
      }
      gcb0 = gcb1
      unicodeScalars.formSuccessor(&start)
    }
    
    return start._position - startIndexUTF16
  }
  
  /// Returns the length of the previous extended grapheme cluster in UTF-16
  /// code units.
  @warn_unused_result
  @inline(never)
  internal func _measureExtendedGraphemeClusterBackward(
    from end: UnicodeScalarView.Index
    ) -> Int {
    var start = end
    start._position = 0
    if start == end {
      return 0
    }
    
    let endIndexUTF16 = end._position
    let unicodeScalars = UnicodeScalarView(_core, offset: _coreOffset)
    let graphemeClusterBreakProperty =
      _UnicodeGraphemeClusterBreakPropertyTrie()
    let segmenter = _UnicodeExtendedGraphemeClusterSegmenter()
    
    var graphemeClusterStart = end
    
    unicodeScalars.formPredecessor(&graphemeClusterStart)
    var gcb0 = graphemeClusterBreakProperty.getPropertyRawValue(
      unicodeScalars[graphemeClusterStart].value)
    
    var graphemeClusterStartUTF16 = graphemeClusterStart._position
    
    while graphemeClusterStart != start {
      unicodeScalars.formPredecessor(&graphemeClusterStart)
      let gcb1 = graphemeClusterBreakProperty.getPropertyRawValue(
        unicodeScalars[graphemeClusterStart].value)
      if segmenter.isBoundary(gcb1, gcb0) {
        break
      }
      gcb0 = gcb1
      graphemeClusterStartUTF16 = graphemeClusterStart._position
    }
    
    return endIndexUTF16 - graphemeClusterStartUTF16
  }
  
  /// Access the `Character` at `position`.
  ///
  /// - Precondition: `position` is a valid position in `self` and
  ///   `position != endIndex`.
  public subscript(i: Index) -> Character {
    return Character(String(unicodeScalars[i._base..<i._endBase]))
  }
}

extension String.CharacterView : RangeReplaceableCollection {
  /// Create an empty instance.
  public init() {
    self.init("")
  }

  /// Replace the characters within `bounds` with `newElements`.
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - Complexity: O(`bounds.count`) if `bounds.upperBound
  ///   == self.endIndex` and `newElements.isEmpty`, O(N) otherwise.
  public mutating func replaceSubrange<
    C: Collection where C.Iterator.Element == Character
  >(
    bounds: Range<Index>, with newElements: C
  ) {
    let rawSubRange: Range<Int> =
      bounds.lowerBound._base._position - _coreOffset
      ..< bounds.upperBound._base._position - _coreOffset
    let lazyUTF16 = newElements.lazy.flatMap { $0.utf16 }
    _core.replaceSubrange(rawSubRange, with: lazyUTF16)
  }

  /// Reserve enough space to store `n` ASCII characters.
  ///
  /// - Complexity: O(`n`).
  public mutating func reserveCapacity(n: Int) {
    _core.reserveCapacity(n)
  }

  /// Append `c` to `self`.
  ///
  /// - Complexity: Amortized O(1).
  public mutating func append(c: Character) {
    switch c._representation {
    case .small(let _63bits):
      let bytes = Character._smallValue(_63bits)
      _core.append(contentsOf: Character._SmallUTF16(bytes))
    case .large(_):
      _core.append(String(c)._core)
    }
  }

  /// Append the elements of `newElements` to `self`.
  public mutating func append<
    S : Sequence where S.Iterator.Element == Character
  >(contentsOf newElements: S) {
    reserveCapacity(_core.count + newElements.underestimatedCount)
    for c in newElements {
      self.append(c)
    }
  }

  /// Create an instance containing `characters`.
  public init<
    S : Sequence where S.Iterator.Element == Character
  >(_ characters: S) {
    self = String.CharacterView()
    self.append(contentsOf: characters)
  }
}

// Algorithms
extension String.CharacterView {
  /// Access the characters in `bounds`.
  ///
  /// - Complexity: O(1) unless bridging from Objective-C requires an
  ///   O(N) conversion.
  public subscript(bounds: Range<Index>) -> String.CharacterView {
    let unicodeScalarRange =
      bounds.lowerBound._base..<bounds.upperBound._base
    return String.CharacterView(
      unicodeScalars[unicodeScalarRange]._core,
      offset: bounds.lowerBound._base._position)
  }
}

extension String.CharacterView {
  @available(*, unavailable, renamed: "replaceSubrange")
  public mutating func replaceRange<
    C : Collection where C.Iterator.Element == Character
  >(
    subRange: Range<Index>, with newElements: C
  ) {
    fatalError("unavailable function can't be called")
  }

  @available(*, unavailable, renamed: "append(contentsOf:)")
  public mutating func appendContentsOf<
    S : Sequence where S.Iterator.Element == Character
  >(newElements: S) {
    fatalError("unavailable function can't be called")
  }
}
