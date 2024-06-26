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

// Implementation notes
// ====================
//
// `Dictionary` uses two storage schemes: native storage and Cocoa storage.
//
// Native storage is a hash table with open addressing and linear probing. The
// bucket array forms a logical ring (e.g., a chain can wrap around the end of
// buckets array to the beginning of it).
//
// The logical bucket array is implemented as three arrays: Key, Value, and a
// bitmap that marks valid entries. An unoccupied entry marks the end of a
// chain.  There is always at least one unoccupied entry among the
// buckets. `Dictionary` does not use tombstones.
//
// In addition to the native storage, `Dictionary` can also wrap an
// `NSDictionary` in order to allow bridging `NSDictionary` to `Dictionary` in
// `O(1)`.
//
// Native dictionary storage uses a data structure like this::
//
//   struct Dictionary<K,V>
//   +------------------------------------------------+
//   | enum Dictionary<K,V>._Variant                  |
//   | +--------------------------------------------+ |
//   | | [struct _NativeDictionary<K,V>             | |
//   | +---|----------------------------------------+ |
//   +----/-------------------------------------------+
//       /
//      |
//      V
//   class __RawDictionaryStorage
//   +-----------------------------------------------------------+
//   | <isa>                                                     |
//   | <refcount>                                                |
//   | _count                                                    |
//   | _capacity                                                 |
//   | _scale                                                    |
//   | _age                                                      |
//   | _seed                                                     |
//   | _rawKeys                                                  |
//   | _rawValue                                                 |
//   | [inline bitset of occupied entries]                       |
//   | [inline array of keys]                                    |
//   | [inline array of values]                                  |
//   +-----------------------------------------------------------+
//
// Cocoa storage uses a data structure like this:
//
//   struct Dictionary<K,V>
//   +----------------------------------------------+
//   | enum Dictionary<K,V>._Variant                |
//   | +----------------------------------------+   |
//   | | [ struct __CocoaDictionary             |   |
//   | +---|------------------------------------+   |
//   +----/-----------------------------------------+
//       /
//      |
//      V
//   class NSDictionary
//   +--------------+
//   | [refcount#1] |
//   | etc.         |
//   +--------------+
//     ^
//     |
//      \  struct __CocoaDictionary.Index
//   +--|------------------------------------+
//   |  * base: __CocoaDictionary            |
//   |  allKeys: array of all keys           |
//   |  currentKeyIndex: index into allKeys  |
//   +---------------------------------------+
//
//
// The Native Kinds of Storage
// ---------------------------
//
// The native backing store is represented by three different classes:
// * `__RawDictionaryStorage`
// * `__EmptyDictionarySingleton` (extends Raw)
// * `_DictionaryStorage<K: Hashable, V>` (extends Raw)
//
// (Hereafter `Raw`, `Empty`, and `Storage`, respectively)
//
// In a less optimized implementation, `Raw` and `Empty` could be eliminated, as
// they exist only to provide special-case behaviors.
//
// `Empty` is the a type-punned empty singleton storage. Its single instance is
// created by the runtime during process startup. Because we use the same
// instance for all empty dictionaries, it cannot declare type parameters.
//
// `Storage` provides backing storage for regular native dictionaries. All
// non-empty native dictionaries use an instance of `Storage` to store their
// elements. `Storage` is a generic class with a nontrivial deinit.
//
// `Raw` is the base class for both `Empty` and `Storage`. It defines a full set
// of ivars to access dictionary contents. Like `Empty`, `Raw` is also
// non-generic; the base addresses it stores are represented by untyped raw
// pointers. The only reason `Raw` exists is to allow `_NativeDictionary` to
// treat `Empty` and `Storage` in a unified way.
//
// Storage classes don't contain much logic; `Raw` in particular is just a
// collection of ivars. `Storage` provides allocation/deinitialization logic,
// while `Empty`/`Storage` implement NSDictionary methods. All other operations
// are actually implemented by the `_NativeDictionary` and `_HashTable` structs.
//
// The `_HashTable` struct provides low-level hash table metadata operations.
// (Lookups, iteration, insertion, removal.) It owns and maintains the
// tail-allocated bitmap.
//
// `_NativeDictionary` implements the actual Dictionary operations.  It
// consists of a reference to a `Raw` instance, to allow for the possibility of
// the empty singleton.
//
//
// Index Invalidation
// ------------------
//
// FIXME: decide if this guarantee is worth making, as it restricts
// collision resolution to first-come-first-serve. The most obvious alternative
// would be robin hood hashing. The Rust code base is the best
// resource on a *practical* implementation of robin hood hashing I know of:
// https://github.com/rust-lang/rust/blob/ac919fcd9d4a958baf99b2f2ed5c3d38a2ebf9d0/src/libstd/collections/hash/map.rs#L70-L178
//
// Indexing a container, `c[i]`, uses the integral offset stored in the index
// to access the elements referenced by the container. Generally, an index into
// one container has no meaning for another. However copy-on-write currently
// preserves indices under insertion, as long as reallocation doesn't occur:
//
//   var (i, found) = d.find(k) // i is associated with d's storage
//   if found {
//      var e = d            // now d is sharing its data with e
//      e[newKey] = newValue // e now has a unique copy of the data
//      return e[i]          // use i to access e
//   }
//
// The result should be a set of iterator invalidation rules familiar to anyone
// familiar with the C++ standard library.  Note that because all accesses to a
// dictionary storage are bounds-checked, this scheme never compromises memory
// safety.
//
// As a safeguard against using invalid indices, Set and Dictionary maintain a
// mutation counter in their storage header (`_age`). This counter gets bumped
// every time an element is removed and whenever the contents are
// rehashed. Native indices include a copy of this counter so that index
// validation can verify it matches with current storage. This can't catch all
// misuse, because counters may match by accident; but it does make indexing a
// lot more reliable.
//
// Bridging
// ========
//
// Bridging `NSDictionary` to `Dictionary`
// ---------------------------------------
//
// FIXME(eager-bridging): rewrite this based on modern constraints.
//
// `NSDictionary` bridges to `Dictionary<NSObject, AnyObject>` in `O(1)`,
// without memory allocation.
//
// Bridging to `Dictionary<AnyHashable, AnyObject>` takes `O(n)` time, as the
// keys need to be fully rehashed after conversion to `AnyHashable`.
//
// Bridging `NSDictionary` to `Dictionary<Key, Value>` is O(1) if both Key and
// Value are bridged verbatim.
//
// Bridging `Dictionary` to `NSDictionary`
// ---------------------------------------
//
// `Dictionary<K, V>` bridges to `NSDictionary` in O(1)
// but may incur an allocation depending on the following conditions:
//
// * If the Dictionary is freshly allocated without any elements, then it
//   contains the empty singleton Storage which is returned as a toll-free
//   implementation of `NSDictionary`.
//
// * If both `K` and `V` are bridged verbatim, then `Dictionary<K, V>` is
//   still toll-free bridged to `NSDictionary` by returning its Storage.
//
// * If the Dictionary is actually a lazily bridged NSDictionary, then that
//   NSDictionary is returned.
//
// * Otherwise, bridging the `Dictionary` is done by wrapping it in a
//   `_SwiftDeferredNSDictionary<K, V>`. This incurs an O(1)-sized allocation.
//
//   Complete bridging of the native Storage's elements to another Storage
//   is performed on first access. This is O(n) work, but is hopefully amortized
//   by future accesses.
//
//   This design ensures that:
//   - Every time keys or values are accessed on the bridged `NSDictionary`,
//     new objects are not created.
//   - Accessing the same element (key or value) multiple times will return
//     the same pointer.
//
// Bridging `NSSet` to `Set` and vice versa
// ----------------------------------------
//
// Bridging guarantees for `Set<Element>` are the same as for
// `Dictionary<Element, NSObject>`.
//

/// A collection whose elements are key-value pairs.
///
/// A dictionary is a type of hash table, providing fast access to the entries
/// it contains. Each entry in the table is identified using its key, which is
/// a hashable type such as a string or number. You use that key to retrieve
/// the corresponding value, which can be any object. In other languages,
/// similar data types are known as hashes or associated arrays.
///
/// Create a new dictionary by using a dictionary literal. A dictionary literal
/// is a comma-separated list of key-value pairs, in which a colon separates
/// each key from its associated value, surrounded by square brackets. You can
/// assign a dictionary literal to a variable or constant or pass it to a
/// function that expects a dictionary.
///
/// Here's how you would create a dictionary of HTTP response codes and their
/// related messages:
///
///     var responseMessages = [200: "OK",
///                             403: "Access forbidden",
///                             404: "File not found",
///                             500: "Internal server error"]
///
/// The `responseMessages` variable is inferred to have type `[Int: String]`.
/// The `Key` type of the dictionary is `Int`, and the `Value` type of the
/// dictionary is `String`.
///
/// To create a dictionary with no key-value pairs, use an empty dictionary
/// literal (`[:]`).
///
///     var emptyDict: [String: String] = [:]
///
/// Any type that conforms to the `Hashable` protocol can be used as a
/// dictionary's `Key` type, including all of Swift's basic types. You can use
/// your own custom types as dictionary keys by making them conform to the
/// `Hashable` protocol.
///
/// Getting and Setting Dictionary Values
/// =====================================
///
/// The most common way to access values in a dictionary is to use a key as a
/// subscript. Subscripting with a key takes the following form:
///
///     print(responseMessages[200])
///     // Prints "Optional("OK")"
///
/// Subscripting a dictionary with a key returns an optional value, because a
/// dictionary might not hold a value for the key that you use in the
/// subscript.
///
/// The next example uses key-based subscripting of the `responseMessages`
/// dictionary with two keys that exist in the dictionary and one that does
/// not.
///
///     let httpResponseCodes = [200, 403, 301]
///     for code in httpResponseCodes {
///         if let message = responseMessages[code] {
///             print("Response \(code): \(message)")
///         } else {
///             print("Unknown response \(code)")
///         }
///     }
///     // Prints "Response 200: OK"
///     // Prints "Response 403: Access forbidden"
///     // Prints "Unknown response 301"
///
/// You can also update, modify, or remove keys and values from a dictionary
/// using the key-based subscript. To add a new key-value pair, assign a value
/// to a key that isn't yet a part of the dictionary.
///
///     responseMessages[301] = "Moved permanently"
///     print(responseMessages[301])
///     // Prints "Optional("Moved permanently")"
///
/// Update an existing value by assigning a new value to a key that already
/// exists in the dictionary. If you assign `nil` to an existing key, the key
/// and its associated value are removed. The following example updates the
/// value for the `404` code to be simply "Not found" and removes the
/// key-value pair for the `500` code entirely.
///
///     responseMessages[404] = "Not found"
///     responseMessages[500] = nil
///     print(responseMessages)
///     // Prints "[301: "Moved permanently", 200: "OK", 403: "Access forbidden", 404: "Not found"]"
///
/// In a mutable `Dictionary` instance, you can modify in place a value that
/// you've accessed through a keyed subscript. The code sample below declares a
/// dictionary called `interestingNumbers` with string keys and values that
/// are integer arrays, then sorts each array in-place in descending order.
///
///     var interestingNumbers = ["primes": [2, 3, 5, 7, 11, 13, 17],
///                               "triangular": [1, 3, 6, 10, 15, 21, 28],
///                               "hexagonal": [1, 6, 15, 28, 45, 66, 91]]
///     for key in interestingNumbers.keys {
///         interestingNumbers[key]?.sort(by: >)
///     }
///
///     print(interestingNumbers["primes"]!)
///     // Prints "[17, 13, 11, 7, 5, 3, 2]"
///
/// Iterating Over the Contents of a Dictionary
/// ===========================================
///
/// Every dictionary is an unordered collection of key-value pairs. You can
/// iterate over a dictionary using a `for`-`in` loop, decomposing each
/// key-value pair into the elements of a tuple.
///
///     let imagePaths = ["star": "/glyphs/star.png",
///                       "portrait": "/images/content/portrait.jpg",
///                       "spacer": "/images/shared/spacer.gif"]
///
///     for (name, path) in imagePaths {
///         print("The path to '\(name)' is '\(path)'.")
///     }
///     // Prints "The path to 'star' is '/glyphs/star.png'."
///     // Prints "The path to 'portrait' is '/images/content/portrait.jpg'."
///     // Prints "The path to 'spacer' is '/images/shared/spacer.gif'."
///
/// The order of key-value pairs in a dictionary is stable between mutations
/// but is otherwise unpredictable. If you need an ordered collection of
/// key-value pairs and don't need the fast key lookup that `Dictionary`
/// provides, see the `KeyValuePairs` type for an alternative.
///
/// You can search a dictionary's contents for a particular value using the
/// `contains(where:)` or `firstIndex(where:)` methods supplied by default
/// implementation. The following example checks to see if `imagePaths` contains
/// any paths in the `"/glyphs"` directory:
///
///     let glyphIndex = imagePaths.firstIndex(where: { $0.value.hasPrefix("/glyphs") })
///     if let index = glyphIndex {
///         print("The '\(imagePaths[index].key)' image is a glyph.")
///     } else {
///         print("No glyphs found!")
///     }
///     // Prints "The 'star' image is a glyph.")
///
/// Note that in this example, `imagePaths` is subscripted using a dictionary
/// index. Unlike the key-based subscript, the index-based subscript returns
/// the corresponding key-value pair as a non-optional tuple.
///
///     print(imagePaths[glyphIndex!])
///     // Prints "(key: "star", value: "/glyphs/star.png")"
///
/// A dictionary's indices stay valid across additions to the dictionary as
/// long as the dictionary has enough capacity to store the added values
/// without allocating more buffer. When a dictionary outgrows its buffer,
/// existing indices may be invalidated without any notification.
///
/// When you know how many new values you're adding to a dictionary, use the
/// `init(minimumCapacity:)` initializer to allocate the correct amount of
/// buffer.
///
/// Bridging Between Dictionary and NSDictionary
/// ============================================
///
/// You can bridge between `Dictionary` and `NSDictionary` using the `as`
/// operator. For bridging to be possible, the `Key` and `Value` types of a
/// dictionary must be classes, `@objc` protocols, or types that bridge to
/// Foundation types.
///
/// Bridging from `Dictionary` to `NSDictionary` always takes O(1) time and
/// space. When the dictionary's `Key` and `Value` types are neither classes
/// nor `@objc` protocols, any required bridging of elements occurs at the
/// first access of each element. For this reason, the first operation that
/// uses the contents of the dictionary may take O(*n*).
///
/// Bridging from `NSDictionary` to `Dictionary` first calls the `copy(with:)`
/// method (`- copyWithZone:` in Objective-C) on the dictionary to get an
/// immutable copy and then performs additional Swift bookkeeping work that
/// takes O(1) time. For instances of `NSDictionary` that are already
/// immutable, `copy(with:)` usually returns the same dictionary in O(1) time;
/// otherwise, the copying performance is unspecified. The instances of
/// `NSDictionary` and `Dictionary` share buffer using the same copy-on-write
/// optimization that is used when two instances of `Dictionary` share
/// buffer.
@frozen
@_eagerMove
public struct Dictionary<Key: Hashable, Value> {
  /// The element type of a dictionary: a tuple containing an individual
  /// key-value pair.
  public typealias Element = (key: Key, value: Value)

  @usableFromInline
  internal var _variant: _Variant

  @inlinable
  internal init(_native: __owned _NativeDictionary<Key, Value>) {
    _variant = _Variant(native: _native)
  }

#if _runtime(_ObjC)
  @inlinable
  internal init(_cocoa: __owned __CocoaDictionary) {
    _variant = _Variant(cocoa: _cocoa)
  }

  /// Private initializer used for bridging.
  ///
  /// Only use this initializer when both conditions are true:
  ///
  /// * it is statically known that the given `NSDictionary` is immutable;
  /// * `Key` and `Value` are bridged verbatim to Objective-C (i.e.,
  ///   are reference types).
  @inlinable
  public // SPI(Foundation)
  init(_immutableCocoaDictionary: __owned AnyObject) {
    _internalInvariant(
      _isBridgedVerbatimToObjectiveC(Key.self) &&
      _isBridgedVerbatimToObjectiveC(Value.self),
      """
      Dictionary can be backed by NSDictionary buffer only when both Key \
      and Value are bridged verbatim to Objective-C
      """)
    self.init(_cocoa: __CocoaDictionary(_immutableCocoaDictionary))
  }
#endif

  /// Creates an empty dictionary.
  @inlinable
  public init() {
    self.init(_native: _NativeDictionary())
  }

  /// Creates an empty dictionary with preallocated space for at least the
  /// specified number of elements.
  ///
  /// Use this initializer to avoid intermediate reallocations of a dictionary's
  /// storage buffer when you know how many key-value pairs you are adding to a
  /// dictionary after creation.
  ///
  /// - Parameter minimumCapacity: The minimum number of key-value pairs that
  ///   the newly created dictionary should be able to store without
  ///   reallocating its storage buffer.
  public // FIXME(reserveCapacity): Should be inlinable
  init(minimumCapacity: Int) {
    _variant = _Variant(native: _NativeDictionary(capacity: minimumCapacity))
  }

  /// Creates a new dictionary from the key-value pairs in the given sequence.
  ///
  /// You use this initializer to create a dictionary when you have a sequence
  /// of key-value tuples with unique keys. Passing a sequence with duplicate
  /// keys to this initializer results in a runtime error. If your
  /// sequence might have duplicate keys, use the
  /// `Dictionary(_:uniquingKeysWith:)` initializer instead.
  ///
  /// The following example creates a new dictionary using an array of strings
  /// as the keys and the integers in a countable range as the values:
  ///
  ///     let digitWords = ["one", "two", "three", "four", "five"]
  ///     let wordToValue = Dictionary(uniqueKeysWithValues: zip(digitWords, 1...5))
  ///     print(wordToValue["three"]!)
  ///     // Prints "3"
  ///     print(wordToValue)
  ///     // Prints "["three": 3, "four": 4, "five": 5, "one": 1, "two": 2]"
  ///
  /// - Parameter keysAndValues: A sequence of key-value pairs to use for
  ///   the new dictionary. Every key in `keysAndValues` must be unique.
  /// - Returns: A new dictionary initialized with the elements of
  ///   `keysAndValues`.
  /// - Precondition: The sequence must not have duplicate keys.
  @inlinable
  public init<S: Sequence>(
    uniqueKeysWithValues keysAndValues: __owned S
  ) where S.Element == (Key, Value) {
    if let d = keysAndValues as? Dictionary<Key, Value> {
      self = d
      return
    }
    var native = _NativeDictionary<Key, Value>(
      capacity: keysAndValues.underestimatedCount)
    // '_MergeError.keyCollision' is caught and handled with an appropriate
    // error message one level down, inside native.merge(_:...). We throw an
    // error instead of calling fatalError() directly because we want the
    // message to include the duplicate key, and the closure only has access to
    // the conflicting values.
    try! native.merge(
      keysAndValues,
      isUnique: true,
      uniquingKeysWith: { _, _ in throw _MergeError.keyCollision })
    self.init(_native: native)
  }

  /// Creates a new dictionary from the key-value pairs in the given sequence,
  /// using a combining closure to determine the value for any duplicate keys.
  ///
  /// You use this initializer to create a dictionary when you have a sequence
  /// of key-value tuples that might have duplicate keys. As the dictionary is
  /// built, the initializer calls the `combine` closure with the current and
  /// new values for any duplicate keys. Pass a closure as `combine` that
  /// returns the value to use in the resulting dictionary: The closure can
  /// choose between the two values, combine them to produce a new value, or
  /// even throw an error.
  ///
  /// The following example shows how to choose the first and last values for
  /// any duplicate keys:
  ///
  ///     let pairsWithDuplicateKeys = [("a", 1), ("b", 2), ("a", 3), ("b", 4)]
  ///
  ///     let firstValues = Dictionary(pairsWithDuplicateKeys,
  ///                                  uniquingKeysWith: { (first, _) in first })
  ///     // ["b": 2, "a": 1]
  ///
  ///     let lastValues = Dictionary(pairsWithDuplicateKeys,
  ///                                 uniquingKeysWith: { (_, last) in last })
  ///     // ["b": 4, "a": 3]
  ///
  /// - Parameters:
  ///   - keysAndValues: A sequence of key-value pairs to use for the new
  ///     dictionary.
  ///   - combine: A closure that is called with the values for any duplicate
  ///     keys that are encountered. The closure returns the desired value for
  ///     the final dictionary.
  @inlinable
  public init<S: Sequence>(
    _ keysAndValues: __owned S,
    uniquingKeysWith combine: (Value, Value) throws -> Value
  ) rethrows where S.Element == (Key, Value) {
    var native = _NativeDictionary<Key, Value>(
      capacity: keysAndValues.underestimatedCount)
    try native.merge(keysAndValues, isUnique: true, uniquingKeysWith: combine)
    self.init(_native: native)
  }

  /// Creates a new dictionary that groups up elements of `values`,
  /// whose values are Arrays of grouped elements,
  /// each keyed by the group key returned by the given closure.
  ///
  /// The arrays in the "values" position of the new dictionary each contain at
  /// least one element, with the elements in the same order as the source
  /// sequence.
  ///
  /// The following example declares an array of names, and then creates a
  /// dictionary from that array by grouping the names by first letter:
  ///
  ///     let students = ["Kofi", "Abena", "Efua", "Kweku", "Akosua"]
  ///     let studentsByLetter = Dictionary(grouping: students, by: { $0.first! })
  ///     // ["E": ["Efua"], "K": ["Kofi", "Kweku"], "A": ["Abena", "Akosua"]]
  ///
  /// The new `studentsByLetter` dictionary has three entries, with students'
  /// names grouped by the keys `"E"`, `"K"`, and `"A"`.
  ///
  /// - Parameters:
  ///   - values: A sequence of values to group into a dictionary.
  ///   - keyForValue: A closure that returns a key for each element in
  ///     `values`.
  @inlinable
  public init<S: Sequence>(
    grouping values: __owned S,
    by keyForValue: (S.Element) throws -> Key
  ) rethrows where Value == [S.Element] {
    try self.init(_native: _NativeDictionary(grouping: values, by: keyForValue))
  }
}

//
// All APIs below should dispatch to `_variant`, without doing any
// additional processing.
//

extension Dictionary: Sequence {
  /// Returns an iterator over the dictionary's key-value pairs.
  ///
  /// Iterating over a dictionary yields the key-value pairs as two-element
  /// tuples. You can decompose the tuple in a `for`-`in` loop, which calls
  /// `makeIterator()` behind the scenes, or when calling the iterator's
  /// `next()` method directly.
  ///
  ///     let hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///     for (name, hueValue) in hues {
  ///         print("The hue of \(name) is \(hueValue).")
  ///     }
  ///     // Prints "The hue of Heliotrope is 296."
  ///     // Prints "The hue of Coral is 16."
  ///     // Prints "The hue of Aquamarine is 156."
  ///
  /// - Returns: An iterator over the dictionary with elements of type
  ///   `(key: Key, value: Value)`.
  @inlinable
  @inline(__always)
  public __consuming func makeIterator() -> Iterator {
    return _variant.makeIterator()
  }
}

// This is not quite Sequence.filter, because that returns [Element], not Self
extension Dictionary {
  /// Returns a new dictionary containing the key-value pairs of the dictionary
  /// that satisfy the given predicate.
  ///
  /// - Parameter isIncluded: A closure that takes a key-value pair as its
  ///   argument and returns a Boolean value indicating whether the pair
  ///   should be included in the returned dictionary.
  /// - Returns: A dictionary of the key-value pairs that `isIncluded` allows.
  @inlinable
  @available(swift, introduced: 4.0)
  public __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> [Key: Value] {
  #if _runtime(_ObjC)
    guard _variant.isNative else {
      // Slow path for bridged dictionaries
      var result = _NativeDictionary<Key, Value>()
      for element in self {
        if try isIncluded(element) {
          result.insertNew(key: element.key, value: element.value)
        }
      }
      return Dictionary(_native: result)
    }
  #endif
    return Dictionary(_native: try _variant.asNative.filter(isIncluded))
  }
}

extension Dictionary: Collection {
  public typealias SubSequence = Slice<Dictionary>
  
  /// The position of the first element in a nonempty dictionary.
  ///
  /// If the collection is empty, `startIndex` is equal to `endIndex`.
  ///
  /// - Complexity: Amortized O(1) if the dictionary does not wrap a bridged
  ///   `NSDictionary`. If the dictionary wraps a bridged `NSDictionary`, the
  ///   performance is unspecified.
  @inlinable
  public var startIndex: Index {
    return _variant.startIndex
  }

  /// The dictionary's "past the end" position---that is, the position one
  /// greater than the last valid subscript argument.
  ///
  /// If the collection is empty, `endIndex` is equal to `startIndex`.
  ///
  /// - Complexity: Amortized O(1) if the dictionary does not wrap a bridged
  ///   `NSDictionary`; otherwise, the performance is unspecified.
  @inlinable
  public var endIndex: Index {
    return _variant.endIndex
  }

  @inlinable
  public func index(after i: Index) -> Index {
    return _variant.index(after: i)
  }

  @inlinable
  public func formIndex(after i: inout Index) {
    _variant.formIndex(after: &i)
  }

  /// Returns the index for the given key.
  ///
  /// If the given key is found in the dictionary, this method returns an index
  /// into the dictionary that corresponds with the key-value pair.
  ///
  ///     let countryCodes = ["BR": "Brazil", "GH": "Ghana", "JP": "Japan"]
  ///     let index = countryCodes.index(forKey: "JP")
  ///
  ///     print("Country code for \(countryCodes[index!].value): '\(countryCodes[index!].key)'.")
  ///     // Prints "Country code for Japan: 'JP'."
  ///
  /// - Parameter key: The key to find in the dictionary.
  /// - Returns: The index for `key` and its associated value if `key` is in
  ///   the dictionary; otherwise, `nil`.
  @inlinable
  @inline(__always)
  public func index(forKey key: Key) -> Index? {
    // Complexity: amortized O(1) for native dictionary, O(*n*) when wrapping an
    // NSDictionary.
    return _variant.index(forKey: key)
  }

  /// Accesses the key-value pair at the specified position.
  ///
  /// This subscript takes an index into the dictionary, instead of a key, and
  /// returns the corresponding key-value pair as a tuple. When performing
  /// collection-based operations that return an index into a dictionary, use
  /// this subscript with the resulting value.
  ///
  /// For example, to find the key for a particular value in a dictionary, use
  /// the `firstIndex(where:)` method.
  ///
  ///     let countryCodes = ["BR": "Brazil", "GH": "Ghana", "JP": "Japan"]
  ///     if let index = countryCodes.firstIndex(where: { $0.value == "Japan" }) {
  ///         print(countryCodes[index])
  ///         print("Japan's country code is '\(countryCodes[index].key)'.")
  ///     } else {
  ///         print("Didn't find 'Japan' as a value in the dictionary.")
  ///     }
  ///     // Prints "(key: "JP", value: "Japan")"
  ///     // Prints "Japan's country code is 'JP'."
  ///
  /// - Parameter position: The position of the key-value pair to access.
  ///   `position` must be a valid index of the dictionary and not equal to
  ///   `endIndex`.
  /// - Returns: A two-element tuple with the key and value corresponding to
  ///   `position`.
  @inlinable
  public subscript(position: Index) -> Element {
    return _variant.lookup(position)
  }

  /// The number of key-value pairs in the dictionary.
  ///
  /// - Complexity: O(1).
  @inlinable
  public var count: Int {
    return _variant.count
  }

  //
  // `Sequence` conformance
  //

  /// A Boolean value that indicates whether the dictionary is empty.
  ///
  /// Dictionaries are empty when created with an initializer or an empty
  /// dictionary literal.
  ///
  ///     var frequencies: [String: Int] = [:]
  ///     print(frequencies.isEmpty)
  ///     // Prints "true"
  @inlinable
  public var isEmpty: Bool {
    return count == 0
  }
}

extension Dictionary {
  /// Accesses the value associated with the given key for reading and writing.
  ///
  /// This *key-based* subscript returns the value for the given key if the key
  /// is found in the dictionary, or `nil` if the key is not found.
  ///
  /// The following example creates a new dictionary and prints the value of a
  /// key found in the dictionary (`"Coral"`) and a key not found in the
  /// dictionary (`"Cerise"`).
  ///
  ///     var hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///     print(hues["Coral"])
  ///     // Prints "Optional(16)"
  ///     print(hues["Cerise"])
  ///     // Prints "nil"
  ///
  /// When you assign a value for a key and that key already exists, the
  /// dictionary overwrites the existing value. If the dictionary doesn't
  /// contain the key, the key and value are added as a new key-value pair.
  ///
  /// Here, the value for the key `"Coral"` is updated from `16` to `18` and a
  /// new key-value pair is added for the key `"Cerise"`.
  ///
  ///     hues["Coral"] = 18
  ///     print(hues["Coral"])
  ///     // Prints "Optional(18)"
  ///
  ///     hues["Cerise"] = 330
  ///     print(hues["Cerise"])
  ///     // Prints "Optional(330)"
  ///
  /// If you assign `nil` as the value for the given key, the dictionary
  /// removes that key and its associated value.
  ///
  /// In the following example, the key-value pair for the key `"Aquamarine"`
  /// is removed from the dictionary by assigning `nil` to the key-based
  /// subscript.
  ///
  ///     hues["Aquamarine"] = nil
  ///     print(hues)
  ///     // Prints "["Coral": 18, "Heliotrope": 296, "Cerise": 330]"
  ///
  /// - Parameter key: The key to find in the dictionary.
  /// - Returns: The value associated with `key` if `key` is in the dictionary;
  ///   otherwise, `nil`.
  @inlinable
  public subscript(key: Key) -> Value? {
    get {
      return _variant.lookup(key)
    }
    set(newValue) {
      if let x = newValue {
        _variant.setValue(x, forKey: key)
      } else {
        removeValue(forKey: key)
      }
    }
    _modify {
      defer { _fixLifetime(self) }
      yield &_variant[key]
    }
  }
}

extension Dictionary: ExpressibleByDictionaryLiteral {
  /// Creates a dictionary initialized with a dictionary literal.
  ///
  /// Do not call this initializer directly. It is called by the compiler to
  /// handle dictionary literals. To use a dictionary literal as the initial
  /// value of a dictionary, enclose a comma-separated list of key-value pairs
  /// in square brackets.
  ///
  /// For example, the code sample below creates a dictionary with string keys
  /// and values.
  ///
  ///     let countryCodes = ["BR": "Brazil", "GH": "Ghana", "JP": "Japan"]
  ///     print(countryCodes)
  ///     // Prints "["BR": "Brazil", "JP": "Japan", "GH": "Ghana"]"
  ///
  /// - Parameter elements: The key-value pairs that will make up the new
  ///   dictionary. Each key in `elements` must be unique.
  @inlinable
  @_semantics("optimize.sil.specialize.generic.size.never")
  public init(dictionaryLiteral elements: (Key, Value)...) {
    let native = _NativeDictionary<Key, Value>(capacity: elements.count)
    for (key, value) in elements {
      let (bucket, found) = native.find(key)
      _precondition(!found, "Dictionary literal contains duplicate keys")
      native._insert(at: bucket, key: key, value: value)
    }
    self.init(_native: native)
  }
}

extension Dictionary {
  /// Accesses the value with the given key, falling back to the given default
  /// value if the key isn't found.
  ///
  /// Use this subscript when you want either the value for a particular key
  /// or, when that key is not present in the dictionary, a default value. This
  /// example uses the subscript with a message to use in case an HTTP response
  /// code isn't recognized:
  ///
  ///     var responseMessages = [200: "OK",
  ///                             403: "Access forbidden",
  ///                             404: "File not found",
  ///                             500: "Internal server error"]
  ///
  ///     let httpResponseCodes = [200, 403, 301]
  ///     for code in httpResponseCodes {
  ///         let message = responseMessages[code, default: "Unknown response"]
  ///         print("Response \(code): \(message)")
  ///     }
  ///     // Prints "Response 200: OK"
  ///     // Prints "Response 403: Access forbidden"
  ///     // Prints "Response 301: Unknown response"
  ///
  /// When a dictionary's `Value` type has value semantics, you can use this
  /// subscript to perform in-place operations on values in the dictionary.
  /// The following example uses this subscript while counting the occurrences
  /// of each letter in a string:
  ///
  ///     let message = "Hello, Elle!"
  ///     var letterCounts: [Character: Int] = [:]
  ///     for letter in message {
  ///         letterCounts[letter, default: 0] += 1
  ///     }
  ///     // letterCounts == ["H": 1, "e": 2, "l": 4, "o": 1, ...]
  ///
  /// When `letterCounts[letter, default: 0] += 1` is executed with a
  /// value of `letter` that isn't already a key in `letterCounts`, the
  /// specified default value (`0`) is returned from the subscript,
  /// incremented, and then added to the dictionary under that key.
  ///
  /// - Note: Do not use this subscript to modify dictionary values if the
  ///   dictionary's `Value` type is a class. In that case, the default value
  ///   and key are not written back to the dictionary after an operation.
  ///
  /// - Parameters:
  ///   - key: The key the look up in the dictionary.
  ///   - defaultValue: The default value to use if `key` doesn't exist in the
  ///     dictionary.
  /// - Returns: The value associated with `key` in the dictionary; otherwise,
  ///   `defaultValue`.
  @inlinable
  public subscript(
    key: Key, default defaultValue: @autoclosure () -> Value
  ) -> Value {
    @inline(__always)
    get {
      return _variant.lookup(key) ?? defaultValue()
    }
    @inline(__always)
    _modify {
      let (bucket, found) = _variant.mutatingFind(key)
      let native = _variant.asNative
      if !found {
        let value = defaultValue()
        native._insert(at: bucket, key: key, value: value)
      }
      let address = native._values + bucket.offset
      defer { _fixLifetime(self) }
      yield &address.pointee
    }
  }

  /// Returns a new dictionary containing the keys of this dictionary with the
  /// values transformed by the given closure.
  ///
  /// - Parameter transform: A closure that transforms a value. `transform`
  ///   accepts each value of the dictionary as its parameter and returns a
  ///   transformed value of the same or of a different type.
  /// - Returns: A dictionary containing the keys and transformed values of
  ///   this dictionary.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the dictionary.
  @inlinable
  public func mapValues<T>(
    _ transform: (Value) throws -> T
  ) rethrows -> Dictionary<Key, T> {
    return try Dictionary<Key, T>(_native: _variant.mapValues(transform))
  }

  /// Returns a new dictionary containing only the key-value pairs that have
  /// non-`nil` values as the result of transformation by the given closure.
  ///
  /// Use this method to receive a dictionary with non-optional values when
  /// your transformation produces optional values.
  ///
  /// In this example, note the difference in the result of using `mapValues`
  /// and `compactMapValues` with a transformation that returns an optional
  /// `Int` value.
  ///
  ///     let data = ["a": "1", "b": "three", "c": "///4///"]
  ///
  ///     let m: [String: Int?] = data.mapValues { str in Int(str) }
  ///     // ["a": Optional(1), "b": nil, "c": nil]
  ///
  ///     let c: [String: Int] = data.compactMapValues { str in Int(str) }
  ///     // ["a": 1]
  ///
  /// - Parameter transform: A closure that transforms a value. `transform`
  ///   accepts each value of the dictionary as its parameter and returns an
  ///   optional transformed value of the same or of a different type.
  /// - Returns: A dictionary containing the keys and non-`nil` transformed
  ///   values of this dictionary.
  ///
  /// - Complexity: O(*m* + *n*), where *n* is the length of the original
  ///   dictionary and *m* is the length of the resulting dictionary.
  @inlinable
  public func compactMapValues<T>(
    _ transform: (Value) throws -> T?
  ) rethrows -> Dictionary<Key, T> {
    let result: _NativeDictionary<Key, T> =
      try self.reduce(into: _NativeDictionary<Key, T>()) { (result, element) in
      if let value = try transform(element.value) {
        result.insertNew(key: element.key, value: value)
      }
    }
    return Dictionary<Key, T>(_native: result)
  }

  /// Updates the value stored in the dictionary for the given key, or adds a
  /// new key-value pair if the key does not exist.
  ///
  /// Use this method instead of key-based subscripting when you need to know
  /// whether the new value supplants the value of an existing key. If the
  /// value of an existing key is updated, `updateValue(_:forKey:)` returns
  /// the original value.
  ///
  ///     var hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///
  ///     if let oldValue = hues.updateValue(18, forKey: "Coral") {
  ///         print("The old value of \(oldValue) was replaced with a new one.")
  ///     }
  ///     // Prints "The old value of 16 was replaced with a new one."
  ///
  /// If the given key is not present in the dictionary, this method adds the
  /// key-value pair and returns `nil`.
  ///
  ///     if let oldValue = hues.updateValue(330, forKey: "Cerise") {
  ///         print("The old value of \(oldValue) was replaced with a new one.")
  ///     } else {
  ///         print("No value was found in the dictionary for that key.")
  ///     }
  ///     // Prints "No value was found in the dictionary for that key."
  ///
  /// - Parameters:
  ///   - value: The new value to add to the dictionary.
  ///   - key: The key to associate with `value`. If `key` already exists in
  ///     the dictionary, `value` replaces the existing associated value. If
  ///     `key` isn't already a key of the dictionary, the `(key, value)` pair
  ///     is added.
  /// - Returns: The value that was replaced, or `nil` if a new key-value pair
  ///   was added.
  @inlinable
  @discardableResult
  public mutating func updateValue(
    _ value: __owned Value,
    forKey key: Key
  ) -> Value? {
    return _variant.updateValue(value, forKey: key)
  }

  /// Merges the key-value pairs in the given sequence into the dictionary,
  /// using a combining closure to determine the value for any duplicate keys.
  ///
  /// Use the `combine` closure to select a value to use in the updated
  /// dictionary, or to combine existing and new values. As the key-value
  /// pairs are merged with the dictionary, the `combine` closure is called
  /// with the current and new values for any duplicate keys that are
  /// encountered.
  ///
  /// This example shows how to choose the current or new values for any
  /// duplicate keys:
  ///
  ///     var dictionary = ["a": 1, "b": 2]
  ///
  ///     // Keeping existing value for key "a":
  ///     dictionary.merge(zip(["a", "c"], [3, 4])) { (current, _) in current }
  ///     // ["b": 2, "a": 1, "c": 4]
  ///
  ///     // Taking the new value for key "a":
  ///     dictionary.merge(zip(["a", "d"], [5, 6])) { (_, new) in new }
  ///     // ["b": 2, "a": 5, "c": 4, "d": 6]
  ///
  /// - Parameters:
  ///   - other:  A sequence of key-value pairs.
  ///   - combine: A closure that takes the current and new values for any
  ///     duplicate keys. The closure returns the desired value for the final
  ///     dictionary.
  @inlinable
  public mutating func merge<S: Sequence>(
    _ other: __owned S,
    uniquingKeysWith combine: (Value, Value) throws -> Value
  ) rethrows where S.Element == (Key, Value) {
    try _variant.merge(other, uniquingKeysWith: combine)
  }

  /// Merges the given dictionary into this dictionary, using a combining
  /// closure to determine the value for any duplicate keys.
  ///
  /// Use the `combine` closure to select a value to use in the updated
  /// dictionary, or to combine existing and new values. As the key-values
  /// pairs in `other` are merged with this dictionary, the `combine` closure
  /// is called with the current and new values for any duplicate keys that
  /// are encountered.
  ///
  /// This example shows how to choose the current or new values for any
  /// duplicate keys:
  ///
  ///     var dictionary = ["a": 1, "b": 2]
  ///
  ///     // Keeping existing value for key "a":
  ///     dictionary.merge(["a": 3, "c": 4]) { (current, _) in current }
  ///     // ["b": 2, "a": 1, "c": 4]
  ///
  ///     // Taking the new value for key "a":
  ///     dictionary.merge(["a": 5, "d": 6]) { (_, new) in new }
  ///     // ["b": 2, "a": 5, "c": 4, "d": 6]
  ///
  /// - Parameters:
  ///   - other:  A dictionary to merge.
  ///   - combine: A closure that takes the current and new values for any
  ///     duplicate keys. The closure returns the desired value for the final
  ///     dictionary.
  @inlinable
  public mutating func merge(
    _ other: __owned [Key: Value],
    uniquingKeysWith combine: (Value, Value) throws -> Value
  ) rethrows {
    try _variant.merge(
      other.lazy.map { ($0, $1) }, uniquingKeysWith: combine)
  }

  /// Creates a dictionary by merging key-value pairs in a sequence into the
  /// dictionary, using a combining closure to determine the value for
  /// duplicate keys.
  ///
  /// Use the `combine` closure to select a value to use in the returned
  /// dictionary, or to combine existing and new values. As the key-value
  /// pairs are merged with the dictionary, the `combine` closure is called
  /// with the current and new values for any duplicate keys that are
  /// encountered.
  ///
  /// This example shows how to choose the current or new values for any
  /// duplicate keys:
  ///
  ///     let dictionary = ["a": 1, "b": 2]
  ///     let newKeyValues = zip(["a", "b"], [3, 4])
  ///
  ///     let keepingCurrent = dictionary.merging(newKeyValues) { (current, _) in current }
  ///     // ["b": 2, "a": 1]
  ///     let replacingCurrent = dictionary.merging(newKeyValues) { (_, new) in new }
  ///     // ["b": 4, "a": 3]
  ///
  /// - Parameters:
  ///   - other:  A sequence of key-value pairs.
  ///   - combine: A closure that takes the current and new values for any
  ///     duplicate keys. The closure returns the desired value for the final
  ///     dictionary.
  /// - Returns: A new dictionary with the combined keys and values of this
  ///   dictionary and `other`.
  @inlinable
  public __consuming func merging<S: Sequence>(
    _ other: __owned S,
    uniquingKeysWith combine: (Value, Value) throws -> Value
  ) rethrows -> [Key: Value] where S.Element == (Key, Value) {
    var result = self
    try result._variant.merge(other, uniquingKeysWith: combine)
    return result
  }

  /// Creates a dictionary by merging the given dictionary into this
  /// dictionary, using a combining closure to determine the value for
  /// duplicate keys.
  ///
  /// Use the `combine` closure to select a value to use in the returned
  /// dictionary, or to combine existing and new values. As the key-value
  /// pairs in `other` are merged with this dictionary, the `combine` closure
  /// is called with the current and new values for any duplicate keys that
  /// are encountered.
  ///
  /// This example shows how to choose the current or new values for any
  /// duplicate keys:
  ///
  ///     let dictionary = ["a": 1, "b": 2]
  ///     let otherDictionary = ["a": 3, "b": 4]
  ///
  ///     let keepingCurrent = dictionary.merging(otherDictionary)
  ///           { (current, _) in current }
  ///     // ["b": 2, "a": 1]
  ///     let replacingCurrent = dictionary.merging(otherDictionary)
  ///           { (_, new) in new }
  ///     // ["b": 4, "a": 3]
  ///
  /// - Parameters:
  ///   - other:  A dictionary to merge.
  ///   - combine: A closure that takes the current and new values for any
  ///     duplicate keys. The closure returns the desired value for the final
  ///     dictionary.
  /// - Returns: A new dictionary with the combined keys and values of this
  ///   dictionary and `other`.
  @inlinable
  public __consuming func merging(
    _ other: __owned [Key: Value],
    uniquingKeysWith combine: (Value, Value) throws -> Value
  ) rethrows -> [Key: Value] {
    var result = self
    try result.merge(other, uniquingKeysWith: combine)
    return result
  }

  /// Removes and returns the key-value pair at the specified index.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// dictionary.
  ///
  /// - Parameter index: The position of the key-value pair to remove. `index`
  ///   must be a valid index of the dictionary, and must not equal the
  ///   dictionary's end index.
  /// - Returns: The key-value pair that correspond to `index`.
  ///
  /// - Complexity: O(*n*), where *n* is the number of key-value pairs in the
  ///   dictionary.
  @inlinable
  @discardableResult
  public mutating func remove(at index: Index) -> Element {
    return _variant.remove(at: index)
  }

  /// Removes the given key and its associated value from the dictionary.
  ///
  /// If the key is found in the dictionary, this method returns the key's
  /// associated value. On removal, this method invalidates all indices with
  /// respect to the dictionary.
  ///
  ///     var hues = ["Heliotrope": 296, "Coral": 16, "Aquamarine": 156]
  ///     if let value = hues.removeValue(forKey: "Coral") {
  ///         print("The value \(value) was removed.")
  ///     }
  ///     // Prints "The value 16 was removed."
  ///
  /// If the key isn't found in the dictionary, `removeValue(forKey:)` returns
  /// `nil`.
  ///
  ///     if let value = hues.removeValue(forKey: "Cerise") {
  ///         print("The value \(value) was removed.")
  ///     } else {
  ///         print("No value found for that key.")
  ///     }
  ///     // Prints "No value found for that key.""
  ///
  /// - Parameter key: The key to remove along with its associated value.
  /// - Returns: The value that was removed, or `nil` if the key was not
  ///   present in the dictionary.
  ///
  /// - Complexity: O(*n*), where *n* is the number of key-value pairs in the
  ///   dictionary.
  @inlinable
  @discardableResult
  public mutating func removeValue(forKey key: Key) -> Value? {
    return _variant.removeValue(forKey: key)
  }

  /// Removes all key-value pairs from the dictionary.
  ///
  /// Calling this method invalidates all indices with respect to the
  /// dictionary.
  ///
  /// - Parameter keepCapacity: Whether the dictionary should keep its
  ///   underlying buffer. If you pass `true`, the operation preserves the
  ///   buffer capacity that the collection has, otherwise the underlying
  ///   buffer is released.  The default is `false`.
  ///
  /// - Complexity: O(*n*), where *n* is the number of key-value pairs in the
  ///   dictionary.
  @inlinable
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    // The 'will not decrease' part in the documentation comment is worded very
    // carefully.  The capacity can increase if we replace Cocoa dictionary with
    // native dictionary.
    _variant.removeAll(keepingCapacity: keepCapacity)
  }
}

extension Dictionary {
  /// A collection containing just the keys of the dictionary.
  ///
  /// When iterated over, keys appear in this collection in the same order as
  /// they occur in the dictionary's key-value pairs. Each key in the keys
  /// collection has a unique value.
  ///
  ///     let countryCodes = ["BR": "Brazil", "GH": "Ghana", "JP": "Japan"]
  ///     print(countryCodes)
  ///     // Prints "["BR": "Brazil", "JP": "Japan", "GH": "Ghana"]"
  ///
  ///     for k in countryCodes.keys {
  ///         print(k)
  ///     }
  ///     // Prints "BR"
  ///     // Prints "JP"
  ///     // Prints "GH"
  @inlinable
  @available(swift, introduced: 4.0)
  public var keys: Keys {
    // FIXME(accessors): Provide a _read
    get {
      return Keys(_dictionary: self)
    }
  }

  /// A collection containing just the values of the dictionary.
  ///
  /// When iterated over, values appear in this collection in the same order as
  /// they occur in the dictionary's key-value pairs.
  ///
  ///     let countryCodes = ["BR": "Brazil", "GH": "Ghana", "JP": "Japan"]
  ///     print(countryCodes)
  ///     // Prints "["BR": "Brazil", "JP": "Japan", "GH": "Ghana"]"
  ///
  ///     for v in countryCodes.values {
  ///         print(v)
  ///     }
  ///     // Prints "Brazil"
  ///     // Prints "Japan"
  ///     // Prints "Ghana"
  @inlinable
  @available(swift, introduced: 4.0)
  public var values: Values {
    // FIXME(accessors): Provide a _read
    get {
      return Values(_dictionary: self)
    }
    _modify {
      var values = Values(_variant: _Variant(dummy: ()))
      swap(&values._variant, &_variant)
      defer { self._variant = values._variant }
      yield &values
    }
  }

  /// A view of a dictionary's keys.
  @frozen
  public struct Keys: Collection, Equatable {
    public typealias Element = Key
    public typealias SubSequence = Slice<Dictionary.Keys>

    @usableFromInline
    internal var _variant: Dictionary<Key, Value>._Variant

    @inlinable
    internal init(_dictionary: __owned Dictionary) {
      self._variant = _dictionary._variant
    }

    // Collection Conformance
    // ----------------------

    @inlinable
    public var startIndex: Index {
      return _variant.startIndex
    }

    @inlinable
    public var endIndex: Index {
      return _variant.endIndex
    }

    @inlinable
    public func index(after i: Index) -> Index {
      return _variant.index(after: i)
    }

    @inlinable
    public func formIndex(after i: inout Index) {
      _variant.formIndex(after: &i)
    }

    @inlinable
    public subscript(position: Index) -> Element {
      return _variant.key(at: position)
    }

    // Customization
    // -------------

    /// The number of keys in the dictionary.
    ///
    /// - Complexity: O(1).
    @inlinable
    public var count: Int {
      return _variant.count
    }

    @inlinable
    public var isEmpty: Bool {
      return count == 0
    }

    @inlinable
    @inline(__always)
    public func _customContainsEquatableElement(_ element: Element) -> Bool? {
      return _variant.contains(element)
    }

    @inlinable
    @inline(__always)
    public func _customIndexOfEquatableElement(_ element: Element) -> Index?? {
      return Optional(_variant.index(forKey: element))
    }

    @inlinable
    @inline(__always)
    public func _customLastIndexOfEquatableElement(_ element: Element) -> Index?? {
      // The first and last elements are the same because each element is unique.
      return _customIndexOfEquatableElement(element)
    }

    @inlinable
    public static func ==(lhs: Keys, rhs: Keys) -> Bool {
      // Equal if the two dictionaries share storage.
#if _runtime(_ObjC)
      if
        lhs._variant.isNative,
        rhs._variant.isNative,
        lhs._variant.asNative._storage === rhs._variant.asNative._storage
      {
        return true
      }
      if
        !lhs._variant.isNative,
        !rhs._variant.isNative,
        lhs._variant.asCocoa.object === rhs._variant.asCocoa.object
      {
        return true
      }
#else
      if lhs._variant.asNative._storage === rhs._variant.asNative._storage {
        return true
      }
#endif

      // Not equal if the dictionaries are different sizes.
      if lhs.count != rhs.count {
        return false
      }

      // Perform unordered comparison of keys.
      for key in lhs {
        if !rhs.contains(key) {
          return false
        }
      }

      return true
    }
  }

  /// A view of a dictionary's values.
  @frozen
  public struct Values: MutableCollection {
    public typealias Element = Value

    @usableFromInline
    internal var _variant: Dictionary<Key, Value>._Variant

    @inlinable
    internal init(_variant: __owned Dictionary<Key, Value>._Variant) {
      self._variant = _variant
    }

    @inlinable
    internal init(_dictionary: __owned Dictionary) {
      self._variant = _dictionary._variant
    }

    // Collection Conformance
    // ----------------------

    @inlinable
    public var startIndex: Index {
      return _variant.startIndex
    }

    @inlinable
    public var endIndex: Index {
      return _variant.endIndex
    }

    @inlinable
    public func index(after i: Index) -> Index {
      return _variant.index(after: i)
    }

    @inlinable
    public func formIndex(after i: inout Index) {
      _variant.formIndex(after: &i)
    }

    @inlinable
    public subscript(position: Index) -> Element {
      // FIXME(accessors): Provide a _read
      get {
        return _variant.value(at: position)
      }
      _modify {
        let native = _variant.ensureUniqueNative()
        let bucket = native.validatedBucket(for: position)
        let address = native._values + bucket.offset
        defer { _fixLifetime(self) }
        yield &address.pointee
      }
    }

    // Customization
    // -------------

    /// The number of values in the dictionary.
    ///
    /// - Complexity: O(1).
    @inlinable
    public var count: Int {
      return _variant.count
    }

    @inlinable
    public var isEmpty: Bool {
      return count == 0
    }

    @inlinable
    public mutating func swapAt(_ i: Index, _ j: Index) {
      guard i != j else { return }
#if _runtime(_ObjC)
      if !_variant.isNative {
        _variant = .init(native: _NativeDictionary(_variant.asCocoa))
      }
#endif
      let isUnique = _variant.isUniquelyReferenced()
      let native = _variant.asNative
      let a = native.validatedBucket(for: i)
      let b = native.validatedBucket(for: j)
      _variant.asNative.swapValuesAt(a, b, isUnique: isUnique)
    }
  }
}

@_unavailableInEmbedded
extension Dictionary.Keys
  : CustomStringConvertible, CustomDebugStringConvertible {
  public var description: String {
    return _makeCollectionDescription()
  }

  public var debugDescription: String {
    return _makeCollectionDescription(withTypeName: "Dictionary.Keys")
  }
}

@_unavailableInEmbedded
extension Dictionary.Values
  : CustomStringConvertible, CustomDebugStringConvertible {
  public var description: String {
    return _makeCollectionDescription()
  }

  public var debugDescription: String {
    return _makeCollectionDescription(withTypeName: "Dictionary.Values")
  }
}

extension Dictionary.Keys {
  @frozen
  public struct Iterator: IteratorProtocol {
    @usableFromInline
    internal var _base: Dictionary<Key, Value>.Iterator

    @inlinable
    @inline(__always)
    internal init(_ base: Dictionary<Key, Value>.Iterator) {
      self._base = base
    }

    @inlinable
    @inline(__always)
    public mutating func next() -> Key? {
#if _runtime(_ObjC)
      if case .cocoa(let cocoa) = _base._variant {
        _base._cocoaPath()
        guard let cocoaKey = cocoa.nextKey() else { return nil }
        return _forceBridgeFromObjectiveC(cocoaKey, Key.self)
      }
#endif
      return _base._asNative.nextKey()
    }
  }

  @inlinable
  @inline(__always)
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_variant.makeIterator())
  }
}

extension Dictionary.Values {
  @frozen
  public struct Iterator: IteratorProtocol {
    @usableFromInline
    internal var _base: Dictionary<Key, Value>.Iterator

    @inlinable
    @inline(__always)
    internal init(_ base: Dictionary<Key, Value>.Iterator) {
      self._base = base
    }

    @inlinable
    @inline(__always)
    public mutating func next() -> Value? {
#if _runtime(_ObjC)
      if case .cocoa(let cocoa) = _base._variant {
        _base._cocoaPath()
        guard let (_, cocoaValue) = cocoa.next() else { return nil }
        return _forceBridgeFromObjectiveC(cocoaValue, Value.self)
      }
#endif
      return _base._asNative.nextValue()
    }
  }

  @inlinable
  @inline(__always)
  public __consuming func makeIterator() -> Iterator {
    return Iterator(_variant.makeIterator())
  }
}

extension Dictionary: Equatable where Value: Equatable {
  @inlinable
  public static func == (lhs: [Key: Value], rhs: [Key: Value]) -> Bool {
#if _runtime(_ObjC)
    switch (lhs._variant.isNative, rhs._variant.isNative) {
    case (true, true):
      return lhs._variant.asNative.isEqual(to: rhs._variant.asNative)
    case (false, false):
      return lhs._variant.asCocoa.isEqual(to: rhs._variant.asCocoa)
    case (true, false):
      return lhs._variant.asNative.isEqual(to: rhs._variant.asCocoa)
    case (false, true):
      return rhs._variant.asNative.isEqual(to: lhs._variant.asCocoa)
    }
#else
    return lhs._variant.asNative.isEqual(to: rhs._variant.asNative)
#endif
  }
}

extension Dictionary: Hashable where Value: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    var commutativeHash = 0
    for (k, v) in self {
      // Note that we use a copy of our own hasher here. This makes hash values
      // dependent on its state, eliminating static collision patterns.
      var elementHasher = hasher
      elementHasher.combine(k)
      elementHasher.combine(v)
      commutativeHash ^= elementHasher._finalize()
    }
    hasher.combine(commutativeHash)
  }
}

@_unavailableInEmbedded
extension Dictionary: _HasCustomAnyHashableRepresentation
where Value: Hashable {
  public __consuming func _toCustomAnyHashable() -> AnyHashable? {
    return AnyHashable(_box: _DictionaryAnyHashableBox(self))
  }
}

@_unavailableInEmbedded
internal struct _DictionaryAnyHashableBox<Key: Hashable, Value: Hashable>
  : _AnyHashableBox {
  internal let _value: Dictionary<Key, Value>
  internal let _canonical: Dictionary<AnyHashable, AnyHashable>

  internal init(_ value: __owned Dictionary<Key, Value>) {
    self._value = value
    self._canonical = value as Dictionary<AnyHashable, AnyHashable>
  }

  internal var _base: Any {
    return _value
  }

  internal var _canonicalBox: _AnyHashableBox {
    return _DictionaryAnyHashableBox<AnyHashable, AnyHashable>(_canonical)
  }

  internal func _isEqual(to other: _AnyHashableBox) -> Bool? {
    guard
      let other = other as? _DictionaryAnyHashableBox<AnyHashable, AnyHashable>
    else {
      return nil
    }
    return _canonical == other._value
  }

  internal var _hashValue: Int {
    return _canonical.hashValue
  }

  internal func _hash(into hasher: inout Hasher) {
    _canonical.hash(into: &hasher)
  }

  internal func _rawHashValue(_seed: Int) -> Int {
    return _canonical._rawHashValue(seed: _seed)
  }

  internal func _unbox<T: Hashable>() -> T? {
    return _value as? T
  }

  internal func _downCastConditional<T>(
    into result: UnsafeMutablePointer<T>
  ) -> Bool {
    guard let value = _value as? T else { return false }
    result.initialize(to: value)
    return true
  }
}

@_unavailableInEmbedded
extension Collection {
  // Utility method for KV collections that wish to implement
  // CustomStringConvertible and CustomDebugStringConvertible using a bracketed
  // list of elements.
  // FIXME: Doesn't use the withTypeName argument yet
  internal func _makeKeyValuePairDescription<K, V>(
    withTypeName type: String? = nil
  ) -> String where Element == (key: K, value: V) {
#if !SWIFT_STDLIB_STATIC_PRINT
    if self.isEmpty {
      return "[:]"
    }
    
    var result = "["
    var first = true
    for (k, v) in self {
      if first {
        first = false
      } else {
        result += ", "
      }
      #if !$Embedded
      debugPrint(k, terminator: "", to: &result)
      result += ": "
      debugPrint(v, terminator: "", to: &result)
      #else
      "(cannot print value in embedded Swift)".write(to: &result)
      result += ": "
      "(cannot print value in embedded Swift)".write(to: &result)
      #endif
    }
    result += "]"
    return result
#else
    return "(collection printing not available)"
#endif
  }
}

@_unavailableInEmbedded
extension Dictionary: CustomStringConvertible, CustomDebugStringConvertible {
  /// A string that represents the contents of the dictionary.
  public var description: String {
    return _makeKeyValuePairDescription()
  }

  /// A string that represents the contents of the dictionary, suitable for
  /// debugging.
  public var debugDescription: String {
    return _makeKeyValuePairDescription()
  }
}

@usableFromInline
@frozen
internal enum _MergeError: Error {
  case keyCollision
}

extension Dictionary {
  /// The position of a key-value pair in a dictionary.
  ///
  /// Dictionary has two subscripting interfaces:
  ///
  /// 1. Subscripting with a key, yielding an optional value:
  ///
  ///        v = d[k]!
  ///
  /// 2. Subscripting with an index, yielding a key-value pair:
  ///
  ///        (k, v) = d[i]
  @frozen
  public struct Index {
    // Index for native dictionary is efficient.  Index for bridged NSDictionary
    // is not, because neither NSEnumerator nor fast enumeration support moving
    // backwards.  Even if they did, there is another issue: NSEnumerator does
    // not support NSCopying, and fast enumeration does not document that it is
    // safe to copy the state.  So, we cannot implement Index that is a value
    // type for bridged NSDictionary in terms of Cocoa enumeration facilities.

    @frozen
    @usableFromInline
    internal enum _Variant {
      case native(_HashTable.Index)
#if _runtime(_ObjC)
      case cocoa(__CocoaDictionary.Index)
#endif
    }

    @usableFromInline
    internal var _variant: _Variant

    @inlinable
    @inline(__always)
    internal init(_variant: __owned _Variant) {
      self._variant = _variant
    }

    @inlinable
    @inline(__always)
    internal init(_native index: _HashTable.Index) {
      self.init(_variant: .native(index))
    }

#if _runtime(_ObjC)
    @inlinable
    @inline(__always)
    internal init(_cocoa index: __owned __CocoaDictionary.Index) {
      self.init(_variant: .cocoa(index))
    }
#endif
  }
}

extension Dictionary.Index._Variant: @unchecked Sendable {}

extension Dictionary.Index {
#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _guaranteedNative: Bool {
    return _canBeClass(Key.self) == 0 || _canBeClass(Value.self) == 0
  }

  // Allow the optimizer to consider the surrounding code unreachable if Element
  // is guaranteed to be native.
  @usableFromInline @_transparent
  internal func _cocoaPath() {
    if _guaranteedNative {
      _conditionallyUnreachable()
    }
  }

  @inlinable
  @inline(__always)
  internal mutating func _isUniquelyReferenced() -> Bool {
    defer { _fixLifetime(self) }
    var handle = _asCocoa.handleBitPattern
    return handle == 0 || _isUnique_native(&handle)
  }

  @usableFromInline @_transparent
  internal var _isNative: Bool {
    switch _variant {
    case .native:
      return true
    case .cocoa:
      _cocoaPath()
      return false
    }
  }
#endif

  @usableFromInline @_transparent
  internal var _asNative: _HashTable.Index {
    switch _variant {
    case .native(let nativeIndex):
      return nativeIndex
#if _runtime(_ObjC)
    case .cocoa:
      _preconditionFailure(
        "Attempting to access Dictionary elements using an invalid index")
#endif
    }
  }

#if _runtime(_ObjC)
  @usableFromInline
  internal var _asCocoa: __CocoaDictionary.Index {
    @_transparent
    get {
      switch _variant {
      case .native:
        _preconditionFailure(
          "Attempting to access Dictionary elements using an invalid index")
      case .cocoa(let cocoaIndex):
        return cocoaIndex
      }
    }
    _modify {
      guard case .cocoa(var cocoa) = _variant else {
        _preconditionFailure(
          "Attempting to access Dictionary elements using an invalid index")
      }
      let dummy = _HashTable.Index(bucket: _HashTable.Bucket(offset: 0), age: 0)
      _variant = .native(dummy)
      defer { _variant = .cocoa(cocoa) }
      yield &cocoa
    }
  }
#endif
}

extension Dictionary.Index: Equatable {
  @inlinable
  public static func == (
    lhs: Dictionary<Key, Value>.Index,
    rhs: Dictionary<Key, Value>.Index
  ) -> Bool {
    switch (lhs._variant, rhs._variant) {
    case (.native(let lhsNative), .native(let rhsNative)):
      return lhsNative == rhsNative
  #if _runtime(_ObjC)
    case (.cocoa(let lhsCocoa), .cocoa(let rhsCocoa)):
      lhs._cocoaPath()
      return lhsCocoa == rhsCocoa
    default:
      _preconditionFailure("Comparing indexes from different dictionaries")
  #endif
    }
  }
}

extension Dictionary.Index: Comparable {
  @inlinable
  public static func < (
    lhs: Dictionary<Key, Value>.Index,
    rhs: Dictionary<Key, Value>.Index
  ) -> Bool {
    switch (lhs._variant, rhs._variant) {
    case (.native(let lhsNative), .native(let rhsNative)):
      return lhsNative < rhsNative
  #if _runtime(_ObjC)
    case (.cocoa(let lhsCocoa), .cocoa(let rhsCocoa)):
      lhs._cocoaPath()
      return lhsCocoa < rhsCocoa
    default:
      _preconditionFailure("Comparing indexes from different dictionaries")
  #endif
    }
  }
}

extension Dictionary.Index: Hashable {
  public // FIXME(cocoa-index): Make inlinable
  func hash(into hasher: inout Hasher) {
#if _runtime(_ObjC)
    guard _isNative else {
      hasher.combine(1 as UInt8)
      hasher.combine(_asCocoa._offset)
      return
    }
    hasher.combine(0 as UInt8)
    hasher.combine(_asNative.bucket.offset)
#else
    hasher.combine(_asNative.bucket.offset)
#endif
  }
}

extension Dictionary {
  /// An iterator over the members of a `Dictionary<Key, Value>`.
  @frozen
  public struct Iterator {
    // Dictionary has a separate IteratorProtocol and Index because of
    // efficiency and implementability reasons.
    //
    // Native dictionaries have efficient indices.
    // Bridged NSDictionary instances don't.
    //
    // Even though fast enumeration is not suitable for implementing
    // Index, which is multi-pass, it is suitable for implementing a
    // IteratorProtocol, which is being consumed as iteration proceeds.

    @usableFromInline
    @frozen
    internal enum _Variant {
      case native(_NativeDictionary<Key, Value>.Iterator)
#if _runtime(_ObjC)
      case cocoa(__CocoaDictionary.Iterator)
#endif
    }

    @usableFromInline
    internal var _variant: _Variant

    @inlinable
    internal init(_variant: __owned _Variant) {
      self._variant = _variant
    }

    @inlinable
    internal init(_native: __owned _NativeDictionary<Key, Value>.Iterator) {
      self.init(_variant: .native(_native))
    }

#if _runtime(_ObjC)
    @inlinable
    internal init(_cocoa: __owned __CocoaDictionary.Iterator) {
      self.init(_variant: .cocoa(_cocoa))
    }
#endif
  }
}

extension Dictionary.Iterator._Variant: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}

extension Dictionary.Iterator {
#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _guaranteedNative: Bool {
    return _canBeClass(Key.self) == 0 || _canBeClass(Value.self) == 0
  }

  /// Allow the optimizer to consider the surrounding code unreachable if
  /// Dictionary<Key, Value> is guaranteed to be native.
  @usableFromInline @_transparent
  internal func _cocoaPath() {
    if _guaranteedNative {
      _conditionallyUnreachable()
    }
  }

  @usableFromInline @_transparent
  internal var _isNative: Bool {
    switch _variant {
    case .native:
      return true
    case .cocoa:
      _cocoaPath()
      return false
    }
  }
#endif

  @usableFromInline @_transparent
  internal var _asNative: _NativeDictionary<Key, Value>.Iterator {
    get {
      switch _variant {
      case .native(let nativeIterator):
        return nativeIterator
#if _runtime(_ObjC)
      case .cocoa:
        _internalInvariantFailure("internal error: does not contain a native index")
#endif
      }
    }
    set {
      self._variant = .native(newValue)
    }
  }

#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var _asCocoa: __CocoaDictionary.Iterator {
    get {
      switch _variant {
      case .native:
        _internalInvariantFailure("internal error: does not contain a Cocoa index")
      case .cocoa(let cocoa):
        return cocoa
      }
    }
  }
#endif

}

extension Dictionary.Iterator: IteratorProtocol {
  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable
  @inline(__always)
  public mutating func next() -> (key: Key, value: Value)? {
#if _runtime(_ObjC)
    guard _isNative else {
      if let (cocoaKey, cocoaValue) = _asCocoa.next() {
        let nativeKey = _forceBridgeFromObjectiveC(cocoaKey, Key.self)
        let nativeValue = _forceBridgeFromObjectiveC(cocoaValue, Value.self)
        return (nativeKey, nativeValue)
      }
      return nil
    }
#endif
    return _asNative.next()
  }
}

#if SWIFT_ENABLE_REFLECTION
extension Dictionary.Iterator: CustomReflectable {
  /// A mirror that reflects the iterator.
  public var customMirror: Mirror {
    return Mirror(
      self,
      children: EmptyCollection<(label: String?, value: Any)>())
  }
}

extension Dictionary: CustomReflectable {
  /// A mirror that reflects the dictionary.
  public var customMirror: Mirror {
    let style = Mirror.DisplayStyle.dictionary
    return Mirror(self, unlabeledChildren: self, displayStyle: style)
  }
}
#endif

extension Dictionary {
  /// Removes and returns the first key-value pair of the dictionary if the
  /// dictionary isn't empty.
  ///
  /// The first element of the dictionary is not necessarily the first element
  /// added. Don't expect any particular ordering of key-value pairs.
  ///
  /// - Returns: The first key-value pair of the dictionary if the dictionary
  ///   is not empty; otherwise, `nil`.
  ///
  /// - Complexity: Averages to O(1) over many calls to `popFirst()`.
  @inlinable
  public mutating func popFirst() -> Element? {
    guard !isEmpty else { return nil }
    return remove(at: startIndex)
  }

  /// The total number of key-value pairs that the dictionary can contain without
  /// allocating new storage.
  @inlinable
  public var capacity: Int {
    return _variant.capacity
  }

  /// Reserves enough space to store the specified number of key-value pairs.
  ///
  /// If you are adding a known number of key-value pairs to a dictionary, use this
  /// method to avoid multiple reallocations. This method ensures that the
  /// dictionary has unique, mutable, contiguous storage, with space allocated
  /// for at least the requested number of key-value pairs.
  ///
  /// Calling the `reserveCapacity(_:)` method on a dictionary with bridged
  /// storage triggers a copy to contiguous storage even if the existing
  /// storage has room to store `minimumCapacity` key-value pairs.
  ///
  /// - Parameter minimumCapacity: The requested number of key-value pairs to
  ///   store.
  public // FIXME(reserveCapacity): Should be inlinable
  mutating func reserveCapacity(_ minimumCapacity: Int) {
    _variant.reserveCapacity(minimumCapacity)
    _internalInvariant(self.capacity >= minimumCapacity)
  }
}

public typealias DictionaryIndex<Key: Hashable, Value> =
  Dictionary<Key, Value>.Index
public typealias DictionaryIterator<Key: Hashable, Value> =
  Dictionary<Key, Value>.Iterator

extension Dictionary: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Keys: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Values: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Keys.Iterator: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Values.Iterator: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Index: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
extension Dictionary.Iterator: @unchecked Sendable
  where Key: Sendable, Value: Sendable {}
