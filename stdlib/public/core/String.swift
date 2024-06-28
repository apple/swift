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

import SwiftShims

@inlinable @_transparent
internal func unimplemented_utf8_32bit(
  _ message: String = "",
  file: StaticString = #file, line: UInt = #line
) -> Never {
  fatalError("32-bit: Unimplemented for UTF-8 support", file: file, line: line)
}

/// A Unicode string value that is a collection of characters.
///
/// A string is a series of characters, such as `"Swift"`, that forms a
/// collection. Strings in Swift are Unicode correct and locale insensitive,
/// and are designed to be efficient. The `String` type bridges with the
/// Objective-C class `NSString` and offers interoperability with C functions
/// that works with strings.
///
/// You can create new strings using string literals or string interpolations.
/// A *string literal* is a series of characters enclosed in quotes.
///
///     let greeting = "Welcome!"
///
/// *String interpolations* are string literals that evaluate any included
/// expressions and convert the results to string form. String interpolations
/// give you an easy way to build a string from multiple pieces. Wrap each
/// expression in a string interpolation in parentheses, prefixed by a
/// backslash.
///
///     let name = "Rosa"
///     let personalizedGreeting = "Welcome, \(name)!"
///     // personalizedGreeting == "Welcome, Rosa!"
///
///     let price = 2
///     let number = 3
///     let cookiePrice = "\(number) cookies: $\(price * number)."
///     // cookiePrice == "3 cookies: $6."
///
/// Combine strings using the concatenation operator (`+`).
///
///     let longerGreeting = greeting + " We're glad you're here!"
///     // longerGreeting == "Welcome! We're glad you're here!"
///
/// Multiline string literals are enclosed in three double quotation marks
/// (`"""`), with each delimiter on its own line. Indentation is stripped from
/// each line of a multiline string literal to match the indentation of the
/// closing delimiter.
///
///     let banner = """
///               __,
///              (           o  /) _/_
///               `.  , , , ,  //  /
///             (___)(_(_/_(_ //_ (__
///                          /)
///                         (/
///             """
///
/// Modifying and Comparing Strings
/// ===============================
///
/// Strings always have value semantics. Modifying a copy of a string leaves
/// the original unaffected.
///
///     var otherGreeting = greeting
///     otherGreeting += " Have a nice time!"
///     // otherGreeting == "Welcome! Have a nice time!"
///
///     print(greeting)
///     // Prints "Welcome!"
///
/// Comparing strings for equality using the equal-to operator (`==`) or a
/// relational operator (like `<` or `>=`) is always performed using Unicode
/// canonical representation. As a result, different representations of a
/// string compare as being equal.
///
///     let cafe1 = "Cafe\u{301}"
///     let cafe2 = "Café"
///     print(cafe1 == cafe2)
///     // Prints "true"
///
/// The Unicode scalar value `"\u{301}"` modifies the preceding character to
/// include an accent, so `"e\u{301}"` has the same canonical representation
/// as the single Unicode scalar value `"é"`.
///
/// Basic string operations are not sensitive to locale settings, ensuring that
/// string comparisons and other operations always have a single, stable
/// result, allowing strings to be used as keys in `Dictionary` instances and
/// for other purposes.
///
/// Accessing String Elements
/// =========================
///
/// A string is a collection of *extended grapheme clusters*, which approximate
/// human-readable characters. Many individual characters, such as "é", "김",
/// and "🇮🇳", can be made up of multiple Unicode scalar values. These scalar
/// values are combined by Unicode's boundary algorithms into extended
/// grapheme clusters, represented by the Swift `Character` type. Each element
/// of a string is represented by a `Character` instance.
///
/// For example, to retrieve the first word of a longer string, you can search
/// for a space and then create a substring from a prefix of the string up to
/// that point:
///
///     let name = "Marie Curie"
///     let firstSpace = name.firstIndex(of: " ") ?? name.endIndex
///     let firstName = name[..<firstSpace]
///     // firstName == "Marie"
///
/// The `firstName` constant is an instance of the `Substring` type---a type
/// that represents substrings of a string while sharing the original string's
/// storage. Substrings present the same interface as strings.
///
///     print("\(name)'s first name has \(firstName.count) letters.")
///     // Prints "Marie Curie's first name has 5 letters."
///
/// Accessing a String's Unicode Representation
/// ===========================================
///
/// If you need to access the contents of a string as encoded in different
/// Unicode encodings, use one of the string's `unicodeScalars`, `utf16`, or
/// `utf8` properties. Each property provides access to a view of the string
/// as a series of code units, each encoded in a different Unicode encoding.
///
/// To demonstrate the different views available for every string, the
/// following examples use this `String` instance:
///
///     let cafe = "Cafe\u{301} du 🌍"
///     print(cafe)
///     // Prints "Café du 🌍"
///
/// The `cafe` string is a collection of the nine characters that are visible
/// when the string is displayed.
///
///     print(cafe.count)
///     // Prints "9"
///     print(Array(cafe))
///     // Prints "["C", "a", "f", "é", " ", "d", "u", " ", "🌍"]"
///
/// Unicode Scalar View
/// -------------------
///
/// A string's `unicodeScalars` property is a collection of Unicode scalar
/// values, the 21-bit codes that are the basic unit of Unicode. Each scalar
/// value is represented by a `Unicode.Scalar` instance and is equivalent to a
/// UTF-32 code unit.
///
///     print(cafe.unicodeScalars.count)
///     // Prints "10"
///     print(Array(cafe.unicodeScalars))
///     // Prints "["C", "a", "f", "e", "\u{0301}", " ", "d", "u", " ", "\u{0001F30D}"]"
///     print(cafe.unicodeScalars.map { $0.value })
///     // Prints "[67, 97, 102, 101, 769, 32, 100, 117, 32, 127757]"
///
/// The `unicodeScalars` view's elements comprise each Unicode scalar value in
/// the `cafe` string. In particular, because `cafe` was declared using the
/// decomposed form of the `"é"` character, `unicodeScalars` contains the
/// scalar values for both the letter `"e"` (101) and the accent character
/// `"´"` (769).
///
/// UTF-16 View
/// -----------
///
/// A string's `utf16` property is a collection of UTF-16 code units, the
/// 16-bit encoding form of the string's Unicode scalar values. Each code unit
/// is stored as a `UInt16` instance.
///
///     print(cafe.utf16.count)
///     // Prints "11"
///     print(Array(cafe.utf16))
///     // Prints "[67, 97, 102, 101, 769, 32, 100, 117, 32, 55356, 57101]"
///
/// The elements of the `utf16` view are the code units for the string when
/// encoded in UTF-16. These elements match those accessed through indexed
/// `NSString` APIs.
///
///     let nscafe = cafe as NSString
///     print(nscafe.length)
///     // Prints "11"
///     print(nscafe.character(at: 3))
///     // Prints "101"
///
/// UTF-8 View
/// ----------
///
/// A string's `utf8` property is a collection of UTF-8 code units, the 8-bit
/// encoding form of the string's Unicode scalar values. Each code unit is
/// stored as a `UInt8` instance.
///
///     print(cafe.utf8.count)
///     // Prints "14"
///     print(Array(cafe.utf8))
///     // Prints "[67, 97, 102, 101, 204, 129, 32, 100, 117, 32, 240, 159, 140, 141]"
///
/// The elements of the `utf8` view are the code units for the string when
/// encoded in UTF-8. This representation matches the one used when `String`
/// instances are passed to C APIs.
///
///     let cLength = strlen(cafe)
///     print(cLength)
///     // Prints "14"
///
/// Measuring the Length of a String
/// ================================
///
/// When you need to know the length of a string, you must first consider what
/// you'll use the length for. Are you measuring the number of characters that
/// will be displayed on the screen, or are you measuring the amount of
/// storage needed for the string in a particular encoding? A single string
/// can have greatly differing lengths when measured by its different views.
///
/// For example, an ASCII character like the capital letter *A* is represented
/// by a single element in each of its four views. The Unicode scalar value of
/// *A* is `65`, which is small enough to fit in a single code unit in both
/// UTF-16 and UTF-8.
///
///     let capitalA = "A"
///     print(capitalA.count)
///     // Prints "1"
///     print(capitalA.unicodeScalars.count)
///     // Prints "1"
///     print(capitalA.utf16.count)
///     // Prints "1"
///     print(capitalA.utf8.count)
///     // Prints "1"
///
/// On the other hand, an emoji flag character is constructed from a pair of
/// Unicode scalar values, like `"\u{1F1F5}"` and `"\u{1F1F7}"`. Each of these
/// scalar values, in turn, is too large to fit into a single UTF-16 or UTF-8
/// code unit. As a result, each view of the string `"🇵🇷"` reports a different
/// length.
///
///     let flag = "🇵🇷"
///     print(flag.count)
///     // Prints "1"
///     print(flag.unicodeScalars.count)
///     // Prints "2"
///     print(flag.utf16.count)
///     // Prints "4"
///     print(flag.utf8.count)
///     // Prints "8"
///
/// To check whether a string is empty, use its `isEmpty` property instead of
/// comparing the length of one of the views to `0`. Unlike with `isEmpty`,
/// calculating a view's `count` property requires iterating through the
/// elements of the string.
///
/// Accessing String View Elements
/// ==============================
///
/// To find individual elements of a string, use the appropriate view for your
/// task. For example, to retrieve the first word of a longer string, you can
/// search the string for a space and then create a new string from a prefix
/// of the string up to that point.
///
///     let name = "Marie Curie"
///     let firstSpace = name.firstIndex(of: " ") ?? name.endIndex
///     let firstName = name[..<firstSpace]
///     print(firstName)
///     // Prints "Marie"
///
/// Strings and their views share indices, so you can access the UTF-8 view of
/// the `name` string using the same `firstSpace` index.
///
///     print(Array(name.utf8[..<firstSpace]))
///     // Prints "[77, 97, 114, 105, 101]"
///
/// Note that an index into one view may not have an exact corresponding
/// position in another view. For example, the `flag` string declared above
/// comprises a single character, but is composed of eight code units when
/// encoded as UTF-8. The following code creates constants for the first and
/// second positions in the `flag.utf8` view. Accessing the `utf8` view with
/// these indices yields the first and second code UTF-8 units.
///
///     let firstCodeUnit = flag.startIndex
///     let secondCodeUnit = flag.utf8.index(after: firstCodeUnit)
///     // flag.utf8[firstCodeUnit] == 240
///     // flag.utf8[secondCodeUnit] == 159
///
/// When used to access the elements of the `flag` string itself, however, the
/// `secondCodeUnit` index does not correspond to the position of a specific
/// character. Instead of only accessing the specific UTF-8 code unit, that
/// index is treated as the position of the character at the index's encoded
/// offset. In the case of `secondCodeUnit`, that character is still the flag
/// itself.
///
///     // flag[firstCodeUnit] == "🇵🇷"
///     // flag[secondCodeUnit] == "🇵🇷"
///
/// If you need to validate that an index from one string's view corresponds
/// with an exact position in another view, use the index's
/// `samePosition(in:)` method or the `init(_:within:)` initializer.
///
///     if let exactIndex = secondCodeUnit.samePosition(in: flag) {
///         print(flag[exactIndex])
///     } else {
///         print("No exact match for this position.")
///     }
///     // Prints "No exact match for this position."
///
/// Performance Optimizations
/// =========================
///
/// Although strings in Swift have value semantics, strings use a copy-on-write
/// strategy to store their data in a buffer. This buffer can then be shared
/// by different copies of a string. A string's data is only copied lazily,
/// upon mutation, when more than one string instance is using the same
/// buffer. Therefore, the first in any sequence of mutating operations may
/// cost O(*n*) time and space.
///
/// When a string's contiguous storage fills up, a new buffer must be allocated
/// and data must be moved to the new storage. String buffers use an
/// exponential growth strategy that makes appending to a string a constant
/// time operation when averaged over many append operations.
///
/// Bridging Between String and NSString
/// ====================================
///
/// Any `String` instance can be bridged to `NSString` using the type-cast
/// operator (`as`), and any `String` instance that originates in Objective-C
/// may use an `NSString` instance as its storage. Because any arbitrary
/// subclass of `NSString` can become a `String` instance, there are no
/// guarantees about representation or efficiency when a `String` instance is
/// backed by `NSString` storage. Because `NSString` is immutable, it is just
/// as though the storage was shared by a copy. The first in any sequence of
/// mutating operations causes elements to be copied into unique, contiguous
/// storage which may cost O(*n*) time and space, where *n* is the length of
/// the string's encoded representation (or more, if the underlying `NSString`
/// has unusual performance characteristics).
///
/// For more information about the Unicode terms used in this discussion, see
/// the [Unicode.org glossary][glossary]. In particular, this discussion
/// mentions [extended grapheme clusters][clusters], [Unicode scalar
/// values][scalars], and [canonical equivalence][equivalence].
///
/// [glossary]: http://www.unicode.org/glossary/
/// [clusters]: http://www.unicode.org/glossary/#extended_grapheme_cluster
/// [scalars]: http://www.unicode.org/glossary/#unicode_scalar_value
/// [equivalence]: http://www.unicode.org/glossary/#canonical_equivalent
@frozen
@_eagerMove
public struct String {
  public // @SPI(Foundation)
  var _guts: _StringGuts

  @inlinable @inline(__always)
  internal init(_ _guts: _StringGuts) {
    self._guts = _guts
    _invariantCheck()
  }

  // This is intentionally a static function and not an initializer, because
  // an initializer would conflict with the Int-parsing initializer, when used
  // as function name, e.g.
  //   [1, 2, 3].map(String.init)
  @_alwaysEmitIntoClient
  @_semantics("string.init_empty_with_capacity")
  @_semantics("inline_late")
  @inlinable
  internal static func _createEmpty(withInitialCapacity: Int) -> String {
    return String(_StringGuts(_initialCapacity: withInitialCapacity))
  }

  /// Creates an empty string.
  ///
  /// Using this initializer is equivalent to initializing a string with an
  /// empty string literal.
  ///
  ///     let empty = ""
  ///     let alsoEmpty = String()
  @inlinable @inline(__always)
  @_semantics("string.init_empty")
  public init() { self.init(_StringGuts()) }
}

extension String: Sendable { }

extension String {
  #if !INTERNAL_CHECKS_ENABLED
  @inlinable @inline(__always) internal func _invariantCheck() {}
  #else
  @usableFromInline @inline(never) @_effects(releasenone)
  internal func _invariantCheck() {
  }
  #endif // INTERNAL_CHECKS_ENABLED

  public func _dump() {
    #if INTERNAL_CHECKS_ENABLED
    _guts._dump()
    #endif // INTERNAL_CHECKS_ENABLED
  }
}

extension String {
  /// Returns a boolean value indicating whether this string is identical to
  /// `other`.
  ///
  /// Two string values are identical if there is no way to distinguish between
  /// them.
  ///
  /// Comparing strings this way includes comparing (normally) hidden
  /// implementation details such as the memory location of any underlying
  /// string storage object. Therefore, identical strings are guaranteed to
  /// compare equal with `==`, but not all equal strings are considered
  /// identical.
  ///
  /// - Performance: O(1)
  @_alwaysEmitIntoClient
  public func _isIdentical(to other: Self) -> Bool {
    self._guts.rawBits == other._guts.rawBits
  }
}

extension String {
  // This force type-casts element to UInt8, since we cannot currently
  // communicate to the type checker that we proved this with our dynamic
  // check in String(decoding:as:).
  @_alwaysEmitIntoClient
  @inline(never) // slow-path
  private static func _fromNonContiguousUnsafeBitcastUTF8Repairing<
    C: Collection
  >(_ input: C) -> (result: String, repairsMade: Bool) {
    _internalInvariant(C.Element.self == UInt8.self)
    return Array(input).withUnsafeBufferPointer {
      let raw = UnsafeRawBufferPointer($0)
      return String._fromUTF8Repairing(raw.bindMemory(to: UInt8.self))
    }
  }


  /// Creates a string from the given Unicode code units in the specified
  /// encoding.
  ///
  /// - Parameters:
  ///   - codeUnits: A collection of code units encoded in the encoding
  ///     specified in `sourceEncoding`.
  ///   - sourceEncoding: The encoding in which `codeUnits` should be
  ///     interpreted.
  @inlinable
  @inline(__always) // Eliminate dynamic type check when possible
  public init<C: Collection, Encoding: Unicode.Encoding>(
    decoding codeUnits: C, as sourceEncoding: Encoding.Type
  ) where C.Iterator.Element == Encoding.CodeUnit {
    guard _fastPath(sourceEncoding == UTF8.self) else {
      self = String._fromCodeUnits(
        codeUnits, encoding: sourceEncoding, repair: true)!.0
      return
    }

    // Fast path for user-defined Collections and typed contiguous collections.
    //
    // Note: this comes first, as the optimizer nearly always has insight into
    // wCSIA, but cannot prove that a type does not have conformance to
    // _HasContiguousBytes.
    if let str = codeUnits.withContiguousStorageIfAvailable({
      (buffer: UnsafeBufferPointer<C.Element>) -> String in
      Builtin.onFastPath() // encourage SIL Optimizer to inline this closure :-(
      let rawBufPtr = UnsafeRawBufferPointer(buffer)
      return String._fromUTF8Repairing(
        UnsafeBufferPointer(
          start: rawBufPtr.baseAddress?.assumingMemoryBound(to: UInt8.self),
          count: rawBufPtr.count)).0
    }) {
      self = str
      return
    }

    #if !$Embedded
    // Fast path for untyped raw storage and known stdlib types
    if let contigBytes = codeUnits as? _HasContiguousBytes,
      contigBytes._providesContiguousBytesNoCopy
    {
      self = contigBytes.withUnsafeBytes { rawBufPtr in
        Builtin.onFastPath() // encourage SIL Optimizer to inline this closure
        return String._fromUTF8Repairing(
          UnsafeBufferPointer(
            start: rawBufPtr.baseAddress?.assumingMemoryBound(to: UInt8.self),
            count: rawBufPtr.count)).0
      }
      return
    }
    #endif

    self = String._fromNonContiguousUnsafeBitcastUTF8Repairing(codeUnits).0
  }

  /// Creates a new string by copying and validating the sequence of
  /// code units passed in, according to the specified encoding.
  ///
  /// This initializer does not try to repair ill-formed code unit sequences.
  /// If any are found, the result of the initializer is `nil`.
  ///
  /// The following example calls this initializer with the contents of two
  /// different arrays---first with a well-formed UTF-8 code unit sequence and
  /// then with an ill-formed UTF-16 code unit sequence.
  ///
  ///     let validUTF8: [UInt8] = [67, 97, 0, 102, 195, 169]
  ///     let valid = String(validating: validUTF8, as: UTF8.self)
  ///     print(valid ?? "nil")
  ///     // Prints "Café"
  ///
  ///     let invalidUTF16: [UInt16] = [0x41, 0x42, 0xd801]
  ///     let invalid = String(validating: invalidUTF16, as: UTF16.self)
  ///     print(invalid ?? "nil")
  ///     // Prints "nil"
  ///
  /// - Parameters:
  ///   - codeUnits: A sequence of code units that encode a `String`
  ///   - encoding: A conformer to `Unicode.Encoding` to be used
  ///               to decode `codeUnits`.
  @inlinable
  @available(SwiftStdlib 6.0, *)
  public init?<Encoding: Unicode.Encoding>(
    validating codeUnits: some Sequence<Encoding.CodeUnit>,
    as encoding: Encoding.Type
  ) {
    let contiguousResult = codeUnits.withContiguousStorageIfAvailable {
      String._validate($0, as: Encoding.self)
    }
    if let validationResult = contiguousResult {
      guard let validatedString = validationResult else {
        return nil
      }
      self = validatedString
      return
    }

    // slow-path
    var transcoded: [UTF8.CodeUnit] = []
    transcoded.reserveCapacity(codeUnits.underestimatedCount)
    var isASCII = true
    let error = transcode(
      codeUnits.makeIterator(),
      from: Encoding.self,
      to: UTF8.self,
      stoppingOnError: true,
      into: {
        uint8 in
        transcoded.append(uint8)
        if isASCII && (uint8 & 0x80) == 0x80 { isASCII = false }
      }
    )
    if error { return nil }
    self = transcoded.withUnsafeBufferPointer{
      String._uncheckedFromUTF8($0, asciiPreScanResult: isASCII)
    }
  }

  /// Creates a new string by copying and validating the sequence of
  /// code units passed in, according to the specified encoding.
  ///
  /// This initializer does not try to repair ill-formed code unit sequences.
  /// If any are found, the result of the initializer is `nil`.
  ///
  /// The following example calls this initializer with the contents of two
  /// different arrays---first with a well-formed UTF-8 code unit sequence and
  /// then with an ill-formed ASCII code unit sequence.
  ///
  ///     let validUTF8: [Int8] = [67, 97, 0, 102, -61, -87]
  ///     let valid = String(validating: validUTF8, as: UTF8.self)
  ///     print(valid ?? "nil")
  ///     // Prints "Café"
  ///
  ///     let invalidASCII: [Int8] = [67, 97, -5]
  ///     let invalid = String(validating: invalidASCII, as: Unicode.ASCII.self)
  ///     print(invalid ?? "nil")
  ///     // Prints "nil"
  ///
  /// - Parameters:
  ///   - codeUnits: A sequence of code units that encode a `String`
  ///   - encoding: A conformer to `Unicode.Encoding` that can decode
  ///               `codeUnits` as `UInt8`
  @inlinable
  @available(SwiftStdlib 6.0, *)
  public init?<Encoding>(
    validating codeUnits: some Sequence<Int8>,
    as encoding: Encoding.Type
  ) where Encoding: Unicode.Encoding, Encoding.CodeUnit == UInt8 {
    let contiguousResult = codeUnits.withContiguousStorageIfAvailable {
      $0.withMemoryRebound(to: UInt8.self) {
        String._validate($0, as: Encoding.self)
      }
    }
    if let validationResult = contiguousResult {
      guard let validatedString = validationResult else {
        return nil
      }
      self = validatedString
      return
    }

    // slow-path
    let uint8s = codeUnits.lazy.map(UInt8.init(bitPattern:))
    self.init(validating: uint8s, as: Encoding.self)
  }

  /// Creates a new string with the specified capacity in UTF-8 code units, and
  /// then calls the given closure with a buffer covering the string's
  /// uninitialized memory.
  ///
  /// The closure should return the number of initialized code units,
  /// or 0 if it couldn't initialize the buffer (for example if the
  /// requested capacity was too small).
  ///
  /// This method replaces ill-formed UTF-8 sequences with the Unicode
  /// replacement character (`"\u{FFFD}"`). This may require resizing
  /// the buffer beyond its original capacity.
  ///
  /// The following examples use this initializer with the contents of two
  /// different `UInt8` arrays---the first with a well-formed UTF-8 code unit
  /// sequence, and the second with an ill-formed sequence at the end.
  ///
  ///     let validUTF8: [UInt8] = [0x43, 0x61, 0x66, 0xC3, 0xA9]
  ///     let invalidUTF8: [UInt8] = [0x43, 0x61, 0x66, 0xC3]
  ///
  ///     let cafe1 = String(unsafeUninitializedCapacity: validUTF8.count) {
  ///         _ = $0.initialize(from: validUTF8)
  ///         return validUTF8.count
  ///     }
  ///     // cafe1 == "Café"
  ///
  ///     let cafe2 = String(unsafeUninitializedCapacity: invalidUTF8.count) {
  ///         _ = $0.initialize(from: invalidUTF8)
  ///         return invalidUTF8.count
  ///     }
  ///     // cafe2 == "Caf�"
  ///
  ///     let empty = String(unsafeUninitializedCapacity: 16) { _ in
  ///         // Can't initialize the buffer (e.g. the capacity is too small).
  ///         return 0
  ///     }
  ///     // empty == ""
  ///
  /// - Parameters:
  ///   - capacity: The number of UTF-8 code units worth of memory to allocate
  ///     for the string (excluding the null terminator).
  ///   - initializer: A closure that accepts a buffer covering uninitialized
  ///     memory with room for `capacity` UTF-8 code units, initializes
  ///     that memory, and returns the number of initialized elements.
  @inline(__always)
  @available(SwiftStdlib 5.3, *)
  public init(
    unsafeUninitializedCapacity capacity: Int,
    initializingUTF8With initializer: (
      _ buffer: UnsafeMutableBufferPointer<UInt8>
    ) throws -> Int
  ) rethrows {
    self = try String(
      _uninitializedCapacity: capacity,
      initializingUTF8With: initializer
    )
  }

  @inline(__always)
  internal init(
    _uninitializedCapacity capacity: Int,
    initializingUTF8With initializer: (
      _ buffer: UnsafeMutableBufferPointer<UInt8>
    ) throws -> Int
  ) rethrows {
    if _fastPath(capacity <= _SmallString.capacity) {
      let smol = try _SmallString(initializingUTF8With: {
        try initializer(.init(start: $0.baseAddress, count: capacity))
      })
      // Fast case where we fit in a _SmallString and don't need UTF8 validation
      if _fastPath(smol.isASCII) {
        self = String(_StringGuts(smol))
      } else {
        // We succeeded in making a _SmallString, but may need to repair UTF8
        self = smol.withUTF8 { String._fromUTF8Repairing($0).result }
      }
      return
    }

    self = try String._fromLargeUTF8Repairing(
      uninitializedCapacity: capacity,
      initializingWith: initializer)
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
  @inlinable
  @inline(__always) // Eliminate dynamic type check when possible
  public func withCString<Result, TargetEncoding: Unicode.Encoding>(
    encodedAs targetEncoding: TargetEncoding.Type,
    _ body: (UnsafePointer<TargetEncoding.CodeUnit>) throws -> Result
  ) rethrows -> Result {
    if targetEncoding == UTF8.self {
      return try self.withCString {
        (cPtr: UnsafePointer<CChar>) -> Result  in
        _internalInvariant(UInt8.self == TargetEncoding.CodeUnit.self)
        let ptr = UnsafeRawPointer(cPtr).assumingMemoryBound(
          to: TargetEncoding.CodeUnit.self)
        return try body(ptr)
      }
    }
    return try _slowWithCString(encodedAs: targetEncoding, body)
  }

  @usableFromInline @inline(never) // slow-path
  @_effects(releasenone)
  internal func _slowWithCString<Result, TargetEncoding: Unicode.Encoding>(
    encodedAs targetEncoding: TargetEncoding.Type,
    _ body: (UnsafePointer<TargetEncoding.CodeUnit>) throws -> Result
  ) rethrows -> Result {
    var copy = self
    return try copy.withUTF8 { utf8 in
      var arg = Array<TargetEncoding.CodeUnit>()
      arg.reserveCapacity(1 &+ self._guts.count / 4)
      let repaired = transcode(
        utf8.makeIterator(),
        from: UTF8.self,
        to: targetEncoding,
        stoppingOnError: false,
        into: { arg.append($0) })
      arg.append(TargetEncoding.CodeUnit(0))
      _internalInvariant(!repaired)
      return try body(arg)
    }
  }
}

extension String: _ExpressibleByBuiltinUnicodeScalarLiteral {
  @_effects(readonly)
  @inlinable @inline(__always)
  public init(_builtinUnicodeScalarLiteral value: Builtin.Int32) {
    self.init(Unicode.Scalar(_unchecked: UInt32(value)))
  }

  @inlinable @inline(__always)
  public init(_ scalar: Unicode.Scalar) {
    self = scalar.withUTF8CodeUnits { String._uncheckedFromUTF8($0) }
  }
}

extension String: _ExpressibleByBuiltinExtendedGraphemeClusterLiteral {
  @inlinable @inline(__always)
  @_effects(readonly) @_semantics("string.makeUTF8")
  public init(
    _builtinExtendedGraphemeClusterLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
  ) {
    self.init(
      _builtinStringLiteral: start,
      utf8CodeUnitCount: utf8CodeUnitCount,
      isASCII: isASCII)
  }
}

extension String: _ExpressibleByBuiltinStringLiteral {
  @inlinable @inline(__always)
  @_effects(readonly) @_semantics("string.makeUTF8")
  public init(
    _builtinStringLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1
    ) {
    let bufPtr = UnsafeBufferPointer(
      start: UnsafeRawPointer(start).assumingMemoryBound(to: UInt8.self),
      count: Int(utf8CodeUnitCount))
    if let smol = _SmallString(bufPtr) {
      self = String(_StringGuts(smol))
      return
    }
    self.init(_StringGuts(bufPtr, isASCII: Bool(isASCII)))
  }
}

extension String: ExpressibleByStringLiteral {
  /// Creates an instance initialized to the given string value.
  ///
  /// Do not call this initializer directly. It is used by the compiler when you
  /// initialize a string using a string literal. For example:
  ///
  ///     let nextStop = "Clark & Lake"
  ///
  /// This assignment to the `nextStop` constant calls this string literal
  /// initializer behind the scenes.
  @inlinable @inline(__always)
  public init(stringLiteral value: String) {
    self = value
  }
}

extension String: CustomDebugStringConvertible {
  /// A representation of the string that is suitable for debugging.
  public var debugDescription: String {
    func hasBreak(between left: String, and right: Unicode.Scalar) -> Bool {
      // Note: we know `left` ends with an ASCII character, so we only need to
      // look at its last scalar.
      var state = _GraphemeBreakingState()
      return state.shouldBreak(between: left.unicodeScalars.last!, and: right)
    }

    // Prevent unquoted scalars in the string from combining with the opening
    // `"` or the tail of the preceding quoted scalar.
    var result = "\""
    var wantBreak = true // true if next scalar must not combine with the last
    for us in self.unicodeScalars {
      if let escaped = us._escaped(asASCII: false) {
        result += escaped
        wantBreak = true
      } else if wantBreak && !hasBreak(between: result, and: us) {
        result += us.escaped(asASCII: true)
        wantBreak = true
      } else {
        result.unicodeScalars.append(us)
        wantBreak = false
      }
    }
    // Also prevent the last scalar from combining with the closing `"`.
    var suffix = "\"".unicodeScalars
    while !result.isEmpty {
      // Append first scalar of suffix, then check if it combines.
      result.unicodeScalars.append(suffix.first!)
      let i = result.index(before: result.endIndex)
      let j = result.unicodeScalars.index(before: result.endIndex)
      if i >= j {
        // All good; append the rest and we're done.
        result.unicodeScalars.append(contentsOf: suffix.dropFirst())
        break
      }
      // Cancel appending the scalar, then quote the last scalar in `result` and
      // prepend it to `suffix`.
      result.unicodeScalars.removeLast()
      let last = result.unicodeScalars.removeLast()
      suffix.insert(
        contentsOf: last.escaped(asASCII: true).unicodeScalars,
        at: suffix.startIndex)
    }
    return result
  }
}

extension String {
  @inlinable // Forward inlinability to append
  @_effects(readonly) @_semantics("string.concat")
  public static func + (lhs: String, rhs: String) -> String {
    var result = lhs
    result.append(rhs)
    return result
  }

  // String append
  @inlinable // Forward inlinability to append
  @_semantics("string.plusequals")
  public static func += (lhs: inout String, rhs: String) {
    lhs.append(rhs)
  }
}

extension Sequence where Element: StringProtocol {
  /// Returns a new string by concatenating the elements of the sequence,
  /// adding the given separator between each element.
  ///
  /// The following example shows how an array of strings can be joined to a
  /// single, comma-separated string:
  ///
  ///     let cast = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     let list = cast.joined(separator: ", ")
  ///     print(list)
  ///     // Prints "Vivien, Marlon, Kim, Karl"
  ///
  /// - Parameter separator: A string to insert between each of the elements
  ///   in this sequence. The default separator is an empty string.
  /// - Returns: A single, concatenated string.
  @_specialize(where Self == Array<Substring>)
  @_specialize(where Self == Array<String>)
  public func joined(separator: String = "") -> String {
    return _joined(separator: separator)
  }

  @inline(__always) // Pick up @_specialize and devirtualize from two callers
  internal func _joined(separator: String) -> String {
    // A likely-under-estimate, but lets us skip some of the growth curve
    // for large Sequences.
    let underestimatedCap =
      (1 &+ separator._guts.count) &* self.underestimatedCount
    var result = ""
    result.reserveCapacity(underestimatedCap)
    if separator.isEmpty {
      for x in self {
        result.append(x._ephemeralString)
      }
      return result
    }

    var iter = makeIterator()
    if let first = iter.next() {
      result.append(first._ephemeralString)
      while let next = iter.next() {
        result.append(separator)
        result.append(next._ephemeralString)
      }
    }
    return result
  }
}

// This overload is necessary because String now conforms to
// BidirectionalCollection, and there are other `joined` overloads that are
// considered more specific. See Flatten.swift.gyb.
extension BidirectionalCollection where Element == String {
  /// Returns a new string by concatenating the elements of the sequence,
  /// adding the given separator between each element.
  ///
  /// The following example shows how an array of strings can be joined to a
  /// single, comma-separated string:
  ///
  ///     let cast = ["Vivien", "Marlon", "Kim", "Karl"]
  ///     let list = cast.joined(separator: ", ")
  ///     print(list)
  ///     // Prints "Vivien, Marlon, Kim, Karl"
  ///
  /// - Parameter separator: A string to insert between each of the elements
  ///   in this sequence. The default separator is an empty string.
  /// - Returns: A single, concatenated string.
  @_specialize(where Self == Array<String>)
  public func joined(separator: String = "") -> String {
    return _joined(separator: separator)
  }
}

// Unicode algorithms
extension String {
  @inline(__always)
  internal func _uppercaseASCII(_ x: UInt8) -> UInt8 {
    /// A "table" for which ASCII characters need to be upper cased.
    /// To determine which bit corresponds to which ASCII character, subtract 1
    /// from the ASCII value of that character and divide by 2. The bit is set if
    /// that character is a lower case character; otherwise, it's not set.
    let _lowercaseTable: UInt64 =
      0b0001_1111_1111_1111_0000_0000_0000_0000 &<< 32

    // Lookup if it should be shifted in our ascii table, then we subtract 0x20 if
    // it should, 0x0 if not.
    // This code is equivalent to:
    // This code is equivalent to:
    // switch sourcex {
    // case let x where (x >= 0x41 && x <= 0x5a):
    //   return x &- 0x20
    // case let x:
    //   return x
    // }
    let isLower = _lowercaseTable &>> UInt64(((x &- 1) & 0b0111_1111) &>> 1)
    let toSubtract = (isLower & 0x1) &<< 5
    return x &- UInt8(truncatingIfNeeded: toSubtract)
  }

  @inline(__always)
  internal func _lowercaseASCII(_ x: UInt8) -> UInt8 {
    /// A "table" for which ASCII characters need to be lower cased.
    /// To determine which bit corresponds to which ASCII character, subtract 1
    /// from the ASCII value of that character and divide by 2. The bit is set if
    /// that character is a upper case character; otherwise, it's not set.
    let _uppercaseTable: UInt64 =
      0b0000_0000_0000_0000_0001_1111_1111_1111 &<< 32

    // Lookup if it should be shifted in our ascii table, then we add 0x20 if
    // it should, 0x0 if not.
    // This code is equivalent to:
    // This code is equivalent to:
    // switch sourcex {
    // case let x where (x >= 0x41 && x <= 0x5a):
    //   return x &- 0x20
    // case let x:
    //   return x
    // }
    let isUpper = _uppercaseTable &>> UInt64(((x &- 1) & 0b0111_1111) &>> 1)
    let toAdd = (isUpper & 0x1) &<< 5
    return x &+ UInt8(truncatingIfNeeded: toAdd)
  }


  /// Returns a lowercase version of the string.
  ///
  /// Here's an example of transforming a string to all lowercase letters.
  ///
  ///     let cafe = "BBQ Café 🍵"
  ///     print(cafe.lowercased())
  ///     // Prints "bbq café 🍵"
  ///
  /// - Returns: A lowercase copy of the string.
  ///
  /// - Complexity: O(*n*)
  @_effects(releasenone)
  public func lowercased() -> String {
    if _fastPath(_guts.isFastASCII) {
      return _guts.withFastUTF8 { utf8 in
        return String(_uninitializedCapacity: utf8.count) { buffer in
          for i in 0 ..< utf8.count {
            buffer[i] = _lowercaseASCII(utf8[i])
          }
          return utf8.count
        }
      }
    }

    var result = ""
    result.reserveCapacity(utf8.count)

    for scalar in unicodeScalars {
      result += scalar.properties.lowercaseMapping
    }

    return result
  }

  /// Returns an uppercase version of the string.
  ///
  /// The following example transforms a string to uppercase letters:
  ///
  ///     let cafe = "Café 🍵"
  ///     print(cafe.uppercased())
  ///     // Prints "CAFÉ 🍵"
  ///
  /// - Returns: An uppercase copy of the string.
  ///
  /// - Complexity: O(*n*)
  @_effects(releasenone)
  public func uppercased() -> String {
    if _fastPath(_guts.isFastASCII) {
      return _guts.withFastUTF8 { utf8 in
        return String(_uninitializedCapacity: utf8.count) { buffer in
          for i in 0 ..< utf8.count {
            buffer[i] = _uppercaseASCII(utf8[i])
          }
          return utf8.count
        }
      }
    }

    var result = ""
    result.reserveCapacity(utf8.count)

    for scalar in unicodeScalars {
      result += scalar.properties.uppercaseMapping
    }

    return result
  }

  /// Creates an instance from the description of a given
  /// `LosslessStringConvertible` instance.
  @inlinable @inline(__always)
  public init<T: LosslessStringConvertible>(_ value: T) {
    self = value.description
  }
}

extension String: CustomStringConvertible {
  /// The value of this string.
  ///
  /// Using this property directly is discouraged. Instead, use simple
  /// assignment to create a new constant or variable equal to this string.
  @inlinable
  public var description: String { return self }
}

extension String {
  public // @testable
  var _nfcCodeUnits: [UInt8] {
    var codeUnits = [UInt8]()
    _withNFCCodeUnits {
      codeUnits.append($0)
    }
    return codeUnits
  }

  public // @testable
  func _withNFCCodeUnits(_ f: (UInt8) throws -> Void) rethrows {
    try _gutsSlice._withNFCCodeUnits(f)
  }
}

extension _StringGutsSlice {
  internal func _isScalarNFCQC(
    _ scalar: Unicode.Scalar,
    _ prevCCC: inout UInt8
  ) -> Bool {
    let normData = Unicode._NormData(scalar, fastUpperbound: 0x300)

    if prevCCC > normData.ccc, normData.ccc != 0 {
      return false
    }

    if !normData.isNFCQC {
      return false
    }

    prevCCC = normData.ccc
    return true
  }

  internal func _withNFCCodeUnits(_ f: (UInt8) throws -> Void) rethrows {
    let substring = String(_guts)[range]
    // Fast path: If we're already NFC (or ASCII), then we don't need to do
    // anything at all.
    if _fastPath(_guts.isNFC) {
      try substring.utf8.forEach(f)
      return
    }

    var isNFCQC = true
    var prevCCC: UInt8 = 0

    if _guts.isFastUTF8 {
      _fastNFCCheck(&isNFCQC, &prevCCC)

      // Because we have access to the fastUTF8, we can go through that instead
      // of accessing the UTF8 view on String.
      if isNFCQC {
        try withFastUTF8 {
          for byte in $0 {
            try f(byte)
          }
        }

        return
      }
    } else {
      for scalar in substring.unicodeScalars {
        if !_isScalarNFCQC(scalar, &prevCCC) {
          isNFCQC = false
          break
        }
      }

      if isNFCQC {
        for byte in substring.utf8 {
          try f(byte)
        }

        return
      }
    }

    for scalar in substring.unicodeScalars._internalNFC {
      try scalar.withUTF8CodeUnits {
        for byte in $0 {
          try f(byte)
        }
      }
    }
  }

  internal func _fastNFCCheck(_ isNFCQC: inout Bool, _ prevCCC: inout UInt8) {
    withFastUTF8 { utf8 in
      var position = 0

      while position < utf8.count {
        // If our first byte is less than 0xCC, then it means we're under the
        // 0x300 scalar value and everything up to 0x300 is NFC already.
        if utf8[position] < 0xCC {
          // If our first byte is less than 0xC0, then it means it is ASCII
          // and only takes up a single byte.
          if utf8[position] < 0xC0 {
            position &+= 1
          } else {
            // Otherwise, this is a 2 byte < 0x300 sequence.
            position &+= 2
          }
          // ASCII always has ccc of 0.
          prevCCC = 0

          continue
        }

        let (scalar, len) = _decodeScalar(utf8, startingAt: position)

        if !_isScalarNFCQC(scalar, &prevCCC) {
          isNFCQC = false
          return
        }

        position &+= len
      }
    }
  }
}
