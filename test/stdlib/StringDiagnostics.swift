// RUN: %target-typecheck-verify-swift

// REQUIRES: objc_interop

import Foundation

// Common pitfall: trying to subscript a string with integers.
func testIntSubscripting(s: String, i: Int) {
  // FIXME swift-3-indexing-model: test new overloads of ..<, ...
  _ = s[i] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an Int, use a String.Index instead.}}
  _ = s[17] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an Int, use a String.Index instead.}}
  _ = s[i...i] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[17..<20] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[17...20] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}

  _ = s[Range(i...i)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[Range(17..<20)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[Range(17...20)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}

  _ = s[Range(i...i)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[Range(17..<20)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
  _ = s[Range(17...20)] // expected-error{{'subscript(_:)' is unavailable: cannot subscript String with an integer range, use a String.Index range instead.}}
}

func testNonAmbiguousStringComparisons() {
  let s1 = "a"
  let s2 = "b"
  var x = false // expected-warning {{variable 'x' was written to, but never read}}
  x = s1 > s2
  x = s1 as String > s2
}

func testAmbiguousStringComparisons(s: String) {
  let nsString = s as NSString
  let a1 = s as NSString == nsString
  let a2 = s as NSString != nsString
  let a3 = s < nsString // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{24-24= as String}}
  let a4 = s <= nsString // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{25-25= as String}}
  let a5 = s >= nsString // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{25-25= as String}}
  let a6 = s > nsString // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{24-24= as String}}
  // now the other way
  let a7 = nsString == s as NSString
  let a8 = nsString != s as NSString
  let a9 = nsString < s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{20-20= as String}}
  let a10 = nsString <= s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{21-21= as String}}
  let a11 = nsString >= s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{21-21= as String}}
  let a12 = nsString > s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}} {{21-21= as String}}
  
  // Shouldn't suggest 'as' in a pattern-matching context, as opposed to all these other situations
  if case nsString = "" {} // expected-error{{expression pattern of type 'NSString' cannot match values of type 'String'}}
}

func testStringDeprecation(hello: String) {
  let hello2 = hello
    .addingPercentEscapes(using: .utf8) // expected-error{{'addingPercentEscapes(using:)' is unavailable}}

  _ = hello2?
    .replacingPercentEscapes(using: .utf8) // expected-error{{'replacingPercentEscapes(using:)' is unavailable}}
}

// Positive and negative tests for String collection types. Testing the complete
// set of conformance just in case protocol hierarchy changes.
func acceptsCollection<C: Collection>(_: C) {}
func acceptsBidirectionalCollection<C: BidirectionalCollection>(_: C) {}
func acceptsRandomAccessCollection<C: RandomAccessCollection>(_: C) {}
// expected-note@-1 {{where 'C' = 'String.UTF8View'}}
// expected-note@-2 {{where 'C' = 'String.UnicodeScalarView'}}
// expected-note@-3 {{where 'C' = 'String.UTF16View'}}
// expected-note@-4 {{where 'C' = 'String'}}

func testStringCollectionTypes(s: String) {
  acceptsCollection(s.utf8)
  acceptsBidirectionalCollection(s.utf8) 
  acceptsRandomAccessCollection(s.utf8) // expected-error{{global function 'acceptsRandomAccessCollection' requires that 'String.UTF8View' conform to 'RandomAccessCollection'}}

  acceptsCollection(s.utf16) 
  acceptsBidirectionalCollection(s.utf16)
  acceptsRandomAccessCollection(s.utf16) // expected-error{{global function 'acceptsRandomAccessCollection' requires that 'String.UTF16View' conform to 'RandomAccessCollection'}}

  acceptsCollection(s.unicodeScalars)
  acceptsBidirectionalCollection(s.unicodeScalars)
  acceptsRandomAccessCollection(s.unicodeScalars) // expected-error{{global function 'acceptsRandomAccessCollection' requires that 'String.UnicodeScalarView' conform to 'RandomAccessCollection'}}

  acceptsCollection(s)
  acceptsBidirectionalCollection(s)
  acceptsRandomAccessCollection(s) // expected-error{{global function 'acceptsRandomAccessCollection' requires that 'String' conform to 'RandomAccessCollection'}}
}

// In previous versions of Swift, code would accidentally select
// init(stringInterpolationSegment:) when String.init was used in the context
// of a CustomStringConvertible type. This checks that we still have an
// unambiguous overload for them to select.
func testStringInitWithCustomStringConvertible() {
  _ = [(1..<10)].map(String.init)
}
