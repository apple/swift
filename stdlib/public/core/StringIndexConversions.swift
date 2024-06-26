//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

extension String.Index {
  /// Creates an index in the given string that corresponds exactly to the
  /// specified position.
  ///
  /// If the index passed as `sourcePosition` represents the start of an
  /// extended grapheme cluster---the element type of a string---then the
  /// initializer succeeds.
  ///
  /// The following example converts the position of the Unicode scalar `"e"`
  /// into its corresponding position in the string. The character at that
  /// position is the composed `"é"` character.
  ///
  ///     let cafe = "Cafe\u{0301}"
  ///     print(cafe)
  ///     // Prints "Café"
  ///
  ///     let scalarsIndex = cafe.unicodeScalars.firstIndex(of: "e")!
  ///     let stringIndex = String.Index(scalarsIndex, within: cafe)!
  ///
  ///     print(cafe[...stringIndex])
  ///     // Prints "Café"
  ///
  /// If the index passed as `sourcePosition` doesn't have an exact
  /// corresponding position in `target`, the result of the initializer is
  /// `nil`. For example, an attempt to convert the position of the combining
  /// acute accent (`"\u{0301}"`) fails. Combining Unicode scalars do not have
  /// their own position in a string.
  ///
  ///     let nextScalarsIndex = cafe.unicodeScalars.index(after: scalarsIndex)
  ///     let nextStringIndex = String.Index(nextScalarsIndex, within: cafe)
  ///
  ///     print(nextStringIndex)
  ///     // Prints "nil"
  ///
  /// - Parameters:
  ///   - sourcePosition: A position in a view of the `target` parameter.
  ///     `sourcePosition` must be a valid index of at least one of the views
  ///     of `target`.
  ///   - target: The string referenced by the resulting index.
  public init?(_ sourcePosition: String.Index, within target: String) {
    // As a special exception, we allow `sourcePosition` to be an UTF-16 index
    // when `self` is a UTF-8 string (or vice versa), to preserve compatibility
    // with (broken) code that keeps using indices from a bridged string after
    // converting the string to a native representation. Such indices are
    // invalid, but returning nil here can break code that appeared to work fine
    // for ASCII strings in Swift releases prior to 5.7.
    let i = target._guts.ensureMatchingEncoding(sourcePosition)
    guard target._isValidIndex(i) else { return nil }
    self = i._characterAligned
  }

  /// Creates an index in the given string that corresponds exactly to the
  /// specified position.
  ///
  /// If the index passed as `sourcePosition` represents the start of an
  /// extended grapheme cluster---the element type of a string---then the
  /// initializer succeeds.
  ///
  /// The following example converts the position of the Unicode scalar `"e"`
  /// into its corresponding position in the string. The character at that
  /// position is the composed `"é"` character.
  ///
  ///     let cafe = "Cafe\u{0301}"
  ///     print(cafe)
  ///     // Prints "Café"
  ///
  ///     let scalarsIndex = cafe.unicodeScalars.firstIndex(of: "e")!
  ///     let stringIndex = String.Index(scalarsIndex, within: cafe)!
  ///
  ///     print(cafe[...stringIndex])
  ///     // Prints "Café"
  ///
  /// If the index passed as `sourcePosition` doesn't have an exact
  /// corresponding position in `target`, the result of the initializer is
  /// `nil`. For example, an attempt to convert the position of the combining
  /// acute accent (`"\u{0301}"`) fails. Combining Unicode scalars do not have
  /// their own position in a string.
  ///
  ///     let nextScalarsIndex = cafe.unicodeScalars.index(after: scalarsIndex)
  ///     let nextStringIndex = String.Index(nextScalarsIndex, within: cafe)
  ///
  ///     print(nextStringIndex)
  ///     // Prints "nil"
  ///
  /// - Parameters:
  ///   - sourcePosition: A position in a view of the `target` parameter.
  ///     `sourcePosition` must be a valid index of at least one of the views
  ///     of `target`.
  ///   - target: The string referenced by the resulting index.
  @available(SwiftStdlib 5.1, *)
  public init?<S: StringProtocol>(
    _ sourcePosition: String.Index, within target: S
  ) {
    if let str = _specialize(target, for: String.self) {
      self.init(sourcePosition, within: str)
      return
    }
    if let str = _specialize(target, for: Substring.self) {
      // As a special exception, we allow `sourcePosition` to be an UTF-16 index
      // when `self` is a UTF-8 string (or vice versa), to preserve
      // compatibility with (broken) code that keeps using indices from a
      // bridged string after converting the string to a native representation.
      // Such indices are invalid, but returning nil here can break code that
      // appeared to work fine for ASCII strings in Swift releases prior to 5.7.
      let i = str._wholeGuts.ensureMatchingEncoding(sourcePosition)
      guard str._isValidIndex(i) else { return nil }
      self = i
      return
    }
    self.init(sourcePosition, within: String(target))
  }

  /// Returns the position in the given UTF-8 view that corresponds exactly to
  /// this index.
  ///
  /// This example first finds the position of the character `"é"`, and then
  /// uses this method find the same position in the string's `utf8` view.
  ///
  ///     let cafe = "Café"
  ///     if let i = cafe.firstIndex(of: "é") {
  ///         let j = i.samePosition(in: cafe.utf8)!
  ///         print(Array(cafe.utf8[j...]))
  ///     }
  ///     // Prints "[195, 169]"
  ///
  /// - Parameter utf8: The view to use for the index conversion. This index
  ///   must be a valid index of at least one view of the string shared by
  ///   `utf8`.
  /// - Returns: The position in `utf8` that corresponds exactly to this index.
  ///   If this index does not have an exact corresponding position in `utf8`,
  ///   this method returns `nil`. For example, an attempt to convert the
  ///   position of a UTF-16 trailing surrogate returns `nil`.
  public func samePosition(
    in utf8: String.UTF8View
  ) -> String.UTF8View.Index? {
    return String.UTF8View.Index(self, within: utf8)
  }

  /// Returns the position in the given UTF-16 view that corresponds exactly to
  /// this index.
  ///
  /// The index must be a valid index of `String(utf16)`.
  ///
  /// This example first finds the position of the character `"é"` and then
  /// uses this method find the same position in the string's `utf16` view.
  ///
  ///     let cafe = "Café"
  ///     if let i = cafe.firstIndex(of: "é") {
  ///         let j = i.samePosition(in: cafe.utf16)!
  ///         print(cafe.utf16[j])
  ///     }
  ///     // Prints "233"
  ///
  /// - Parameter utf16: The view to use for the index conversion. This index
  ///   must be a valid index of at least one view of the string shared by
  ///   `utf16`.
  /// - Returns: The position in `utf16` that corresponds exactly to this
  ///   index. If this index does not have an exact corresponding position in
  ///   `utf16`, this method returns `nil`. For example, an attempt to convert
  ///   the position of a UTF-8 continuation byte returns `nil`.
  public func samePosition(
    in utf16: String.UTF16View
  ) -> String.UTF16View.Index? {
    return String.UTF16View.Index(self, within: utf16)
  }
}

extension String {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  ///     let cafe = "Cafe\u{301}" // "Café"
  ///     let accent = cafe.unicodeScalars.firstIndex(of: "\u{301")!
  ///     let char = cafe._index(roundingDown: accent)
  ///     print(cafe[char]) // "é"
  ///
  /// `String` methods such as `index(after:)` and `distance(from:to:)`
  /// implicitly round their input indices down to the nearest valid index:
  ///
  ///     let i = cafe.index(before: char)
  ///     let j = cafe.index(before: accent)
  ///     print(cafe[i], cafe[j]) // "f f"
  ///     print(i == j) // true
  ///
  /// This operation lets you perform this rounding yourself. For example, this
  /// can be used to safely check if `index(before:)` would consider some
  /// arbitrary index equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of this string.
  /// - Returns: The largest valid index within this string that doesn't exceed
  ///     `i`.
  @available(SwiftStdlib 5.8, *)
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    _guts.validateInclusiveCharacterIndex(i)
  }
}

extension Substring {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  /// `Substring` methods such as `index(after:)` and `distance(from:to:)`
  /// implicitly round their input indices down to the nearest valid index.
  /// This operation lets you perform this rounding yourself. For example, this
  /// can be used to safely check if `index(before:)` would consider some
  /// arbitrary index equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of this
  ///     substring.
  /// - Returns: The largest valid index within this substring that doesn't
  ///     exceed `i`.
  @available(SwiftStdlib 5.8, *)
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    _wholeGuts.validateInclusiveCharacterIndex(i, in: _bounds)
  }
}

extension String.UnicodeScalarView {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  /// Methods such as `index(after:)` and `distance(from:to:)` implicitly round
  /// their input indices down to the nearest valid index. This operation lets
  /// you perform this rounding yourself. For example, this can be used to
  /// safely check if `index(before:)` would consider some arbitrary index
  /// equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of the string
  ///    shared by this view.
  /// - Returns: The largest valid index within this view that doesn't exceed
  ///     `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    _guts.validateInclusiveScalarIndex(i)
  }
}

extension Substring.UnicodeScalarView {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  /// Methods such as `index(after:)` and `distance(from:to:)` implicitly round
  /// their input indices down to the nearest valid index. This operation lets
  /// you perform this rounding yourself. For example, this can be used to
  /// safely check if `index(before:)` would consider some arbitrary index
  /// equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of the
  ///    substring shared by this view.
  /// - Returns: The largest valid index within this view that doesn't exceed
  ///     `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    _wholeGuts.validateInclusiveScalarIndex(i, in: _bounds)
  }
}

extension String.UTF8View {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  /// Methods such as `index(after:)` and `distance(from:to:)` implicitly round
  /// their input indices down to the nearest valid index. This operation lets
  /// you perform this rounding yourself. For example, this can be used to
  /// safely check if `index(before:)` would consider some arbitrary index
  /// equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of the
  ///    substring shared by this view.
  /// - Returns: The largest valid index within this view that doesn't exceed
  ///     `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    let i = _guts.validateInclusiveSubscalarIndex(i)
    guard _guts.isForeign else { return i.strippingTranscoding._knownUTF8 }
    return _utf8AlignForeignIndex(i)
  }
}

extension Substring.UTF8View {
  /// Returns the largest valid index in `self` that does not exceed the given
  /// position.
  ///
  /// Methods such as `index(after:)` and `distance(from:to:)` implicitly round
  /// their input indices down to the nearest valid index. This operation lets
  /// you perform this rounding yourself. For example, this can be used to
  /// safely check if `index(before:)` would consider some arbitrary index
  /// equivalent to the start index before calling it.
  ///
  /// - Parameter i: An index that is valid in at least one view of the
  ///    substring shared by this view.
  /// - Returns: The largest valid index within this view that doesn't exceed
  ///     `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    let i = _wholeGuts.validateInclusiveSubscalarIndex(i, in: _bounds)
    guard _wholeGuts.isForeign else { return i.strippingTranscoding._knownUTF8 }
    return _slice._base._utf8AlignForeignIndex(i)
  }
}

extension String.UTF16View {
  /// Returns the valid index in `self` that this view considers equivalent to
  /// the given index.
  ///
  /// Indices in the UTF-8 view that address positions between Unicode scalars
  /// are rounded down to the nearest scalar boundary; other indices are left as
  /// is.
  ///
  /// - Parameter i: An index that is valid in at least one view of the
  ///    substring shared by this view.
  /// - Returns: The valid index in `self` that this view considers equivalent
  ///    to `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    let i = _guts.validateInclusiveSubscalarIndex(i)
    if _guts.isForeign { return i.strippingTranscoding._knownUTF16 }
    return _utf16AlignNativeIndex(i)
  }
}

extension Substring.UTF16View {
  /// Returns the valid index in `self` that this view considers equivalent to
  /// the given index.
  ///
  /// Indices in the UTF-8 view that address positions between Unicode scalars
  /// are rounded down to the nearest scalar boundary; other indices are left as
  /// is.
  ///
  /// - Parameter i: An index that is valid in at least one view of the
  ///    substring shared by this view.
  /// - Returns: The valid index in `self` that this view considers equivalent
  ///    to `i`.
  @_alwaysEmitIntoClient
  public // SPI(Foundation) FIXME: This should be API
  func _index(roundingDown i: Index) -> Index {
    let i = _wholeGuts.validateInclusiveSubscalarIndex(i, in: _bounds)
    if _wholeGuts.isForeign { return i.strippingTranscoding._knownUTF16 }
    return _slice._base._utf16AlignNativeIndex(i)
  }
}
