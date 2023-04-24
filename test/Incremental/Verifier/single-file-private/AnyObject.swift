// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios
// UNSUPPORTED: CPU=armv7k && OS=ios
// Exclude iOS-based 32-bit platforms because the Foundation overlays introduce
// an extra dependency on _KeyValueCodingAndObservingPublishing only for 64-bit
// platforms.
// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: cd %t && %target-swift-frontend(mock-sdk: %clang-importer-sdk) -c -module-name main -verify-incremental-dependencies -primary-file %s -o /dev/null

import Foundation

// expected-provides {{LookupFactory}}
// expected-provides {{NSObject}}
// expected-provides {{Selector}}
// expected-provides {{Bool}}
// expected-provides {{ObjCBool}}
// expected-provides {{==}}
// expected-provides {{Equatable}}
// expected-provides {{Hasher}}
// expected-provides {{_ObjectiveCBridgeable}}
// expected-provides{{Hashable}}
// expected-member {{ObjectiveC.NSObject.NSObject}}
// expected-superclass {{ObjectiveC.NSObject}}
// expected-conformance {{ObjectiveC.NSObjectProtocol}}
// expected-member {{ObjectiveC.NSObjectProtocol.NSObject}}
// expected-member {{ObjectiveC.NSObject.Bool}}
// expected-conformance {{Swift.Hashable}}
// expected-conformance {{Swift.Equatable}}
// expected-member {{Swift._ExpressibleByBuiltinIntegerLiteral.init}}
@objc private class LookupFactory: NSObject {
  // expected-provides {{AssignmentPrecedence}}
  // expected-provides {{IntegerLiteralType}}
  // expected-provides {{FloatLiteralType}}
  // expected-provides {{Int}}
  // expected-member {{ObjectiveC.NSObject.someMember}}
  // expected-member {{ObjectiveC.NSObject.Int}}
  // expected-member {{ObjectiveC.NSObjectProtocol.someMember}}
  // expected-member {{ObjectiveC.NSObjectProtocol.Int}}
  // expected-member {{main.LookupFactory.Int}}
  @objc var someMember: Int = 0
  // expected-member {{ObjectiveC.NSObject.someMethod}}
  // expected-member {{ObjectiveC.NSObjectProtocol.someMethod}}
  @objc func someMethod() {}

  // expected-member {{ObjectiveC.NSObject.init}}
  // expected-member {{ObjectiveC.NSObject.deinit}}
  // expected-member {{ObjectiveC.NSObjectProtocol.init}}
  // expected-member {{ObjectiveC.NSObjectProtocol.deinit}}
  // expected-member {{main.LookupFactory.init}}
  // expected-member {{main.LookupFactory.deinit}}
  // expected-member {{main.LookupFactory.someMember}}
  // expected-member {{main.LookupFactory.someMethod}}
}

// expected-member {{Swift.ExpressibleByNilLiteral.callAsFunction}}
// expected-member {{Swift.CustomReflectable.callAsFunction}}
// expected-member {{Swift._ObjectiveCBridgeable.callAsFunction}}
// expected-member {{Swift.Optional<Wrapped>.callAsFunction}}
// expected-member {{Swift.CustomDebugStringConvertible.callAsFunction}}
// expected-member {{Swift.Equatable.callAsFunction}}
// expected-member {{Swift.Hashable.callAsFunction}}
// expected-member {{Swift.Encodable.callAsFunction}}
// expected-member {{Swift.Decodable.callAsFunction}}

// expected-member {{Swift.Hashable._rawHashValue}}
// expected-member {{ObjectiveC.NSObject.hash}}
// expected-member {{Swift.Equatable.hashValue}}
// expected-member{{Swift.Hashable.hashValue}}
// expected-member{{Swift.Hashable.hash}}
// expected-member{{ObjectiveC.NSObjectProtocol.==}}
// expected-member {{ObjectiveC.NSObjectProtocol.hashValue}}
// expected-member {{ObjectiveC.NSObjectProtocol.Hasher}}
// expected-member {{Swift.Equatable._rawHashValue}}
// expected-member {{ObjectiveC.NSObject.hashValue}}
// expected-member {{ObjectiveC.NSObjectProtocol.Bool}}
// expected-member {{ObjectiveC.NSObject.==}}
// expected-member {{Swift.Equatable.==}}
// expected-member {{ObjectiveC.NSObject.Hasher}}
// expected-member {{ObjectiveC.NSObjectProtocol.hash}}

// expected-member {{Swift.Hashable.init}}
// expected-member {{Swift.Hashable.deinit}}
// expected-member {{Swift.Equatable.init}}
// expected-member {{Swift.Equatable.deinit}}

// expected-member {{Swift.Hashable.==}}
// expected-member {{Swift.Equatable.hash}}
// expected-member {{ObjectiveC.NSObject._rawHashValue}}
// expected-member {{ObjectiveC.NSObjectProtocol._rawHashValue}}

// expected-provides {{AnyObject}}
func lookupOnAnyObject(object: AnyObject) { // expected-provides {{lookupOnAnyObject}}
  _ = object.someMember // expected-dynamic-member {{someMember}}
  object.someMethod() // expected-dynamic-member {{someMethod}}
}

// expected-member {{Swift.Hashable.someMethod}}
// expected-member {{Swift.Equatable.someMethod}}
// expected-member {{Swift.Equatable.someMember}}
// expected-member {{Swift.Hashable.someMember}}
// expected-member {{Swift.Sendable.callAsFunction}}
// expected-member {{ObjectiveC.NSObject.someMethodWithDeprecatedOptions}}
// expected-member {{ObjectiveC.NSObject.someMethodWithPotentiallyUnavailableOptions}}
// expected-member {{ObjectiveC.NSObject.someMethodWithUnavailableOptions}}
// expected-member {{ObjectiveC.NSObjectProtocol.someMethodWithUnavailableOptions}}
// expected-member {{ObjectiveC.NSObjectProtocol.someMethodWithPotentiallyUnavailableOptions}}
// expected-member {{ObjectiveC.NSObjectProtocol.someMethodWithDeprecatedOptions}}

// expected-member {{Swift.Optional<Wrapped>.deinit}}
// expected-member {{Swift.Int32.deinit}}
// expected-member {{Swift.Int.deinit}}
// expected-member {{Swift.UnsafeMutablePointer<Pointee>.deinit}}
