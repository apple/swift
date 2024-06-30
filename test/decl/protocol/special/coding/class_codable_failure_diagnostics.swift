// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -parse-as-library -swift-version 4 %s -verify

// Codable class with non-Codable property.
class C1 : Codable {
  // expected-error@-1 {{type 'C1' does not conform to protocol 'Decodable'}}
  // expected-error@-2 {{type 'C1' does not conform to protocol 'Encodable'}}

  class Nested {}
  var a: String = ""
  var b: Int = 0
  var c: Nested = Nested()
  // expected-note@-1 {{cannot automatically synthesize 'Decodable' because 'Nested' does not conform to 'Decodable'}}
  // expected-note@-2 {{cannot automatically synthesize 'Encodable' because 'Nested' does not conform to 'Encodable'}}
}

// Codable class with non-enum CodingKeys.
class C2 : Codable {
  // expected-error@-1 {{type 'C2' does not conform to protocol 'Decodable'}}
  // expected-error@-2 {{type 'C2' does not conform to protocol 'Encodable'}}

  var a: String = ""
  var b: Int = 0
  var c: Double?

  struct CodingKeys : CodingKey {
  // expected-note@-1 {{cannot automatically synthesize 'Decodable' because 'CodingKeys' is not an enum}}
  // expected-note@-2 {{cannot automatically synthesize 'Encodable' because 'CodingKeys' is not an enum}}
    var stringValue = ""
    var intValue = nil
    // expected-error@-1 {{'nil' requires a contextual type}}
    init?(stringValue: String) {}
    init?(intValue: Int) {}
  }
}

// Codable class with CodingKeys not conforming to CodingKey.
class C3 : Codable {
  // expected-error@-1 {{type 'C3' does not conform to protocol 'Decodable'}}
  // expected-error@-2 {{type 'C3' does not conform to protocol 'Encodable'}}

  var a: String = ""
  var b: Int = 0
  var c: Double?

  enum CodingKeys : String {
  // expected-note@-1 {{cannot automatically synthesize 'Decodable' because 'CodingKeys' does not conform to CodingKey}}
  // expected-note@-2 {{cannot automatically synthesize 'Encodable' because 'CodingKeys' does not conform to CodingKey}}
    case a
    case b
    case c
  }
}

// Codable class with extraneous CodingKeys
class C4 : Codable {
  // expected-error@-1 {{type 'C4' does not conform to protocol 'Decodable'}}
  // expected-error@-2 {{type 'C4' does not conform to protocol 'Encodable'}}

  var a: String = ""
  var b: Int = 0
  var c: Double?

  enum CodingKeys : String, CodingKey {
    case a
    case a2
    // expected-note@-1 2{{CodingKey case 'a2' does not match any stored properties}}
    case b
    case b2
    // expected-note@-1 2{{CodingKey case 'b2' does not match any stored properties}}
    case c
    case c2
    // expected-note@-1 2{{CodingKey case 'c2' does not match any stored properties}}
  }
}

// Codable class with non-decoded property (which has no default value).
class C5 : Codable {
  // expected-error@-1 {{type 'C5' does not conform to protocol 'Decodable'}}
  // expected-error@-2 {{class 'C5' has no initializers}}

  var a: String = ""
  var b: Int
  // expected-note@-1 {{cannot automatically synthesize 'Decodable' because 'b' does not have a matching CodingKey and does not have a default value}}
  // expected-note@-2 {{stored property 'b' without initial value prevents synthesized initializers}}
  var c: Double?

  enum CodingKeys : String, CodingKey {
    case a
    case c
  }
}

// Codable class with non-decoded property (which has no default value).
class C6 : Codable {
  // expected-error@-1 {{type 'C6' does not conform to protocol 'Decodable'}}

  var a: String = ""
  var b: Int
  // expected-note@-1 {{cannot automatically synthesize 'Decodable' because 'b' does not have a matching CodingKey and does not have a default value}}
  var c: Double?

  enum CodingKeys : String, CodingKey {
    case a
    case c
  }

  init() {
    b = 5
  }
}

// Non-final classes cannot synthesize Decodable in an extension.
class C7 {}
extension C7 : Decodable {}
// expected-error@-1 {{implementation of 'Decodable' for non-final class cannot be automatically synthesized in extension because initializer requirement 'init(from:)' can only be satisfied by a 'required' initializer in the class definition}}

// Check that the diagnostics from an extension end up on the extension
class C8 {
// expected-note@-1 {{cannot automatically synthesize 'init(from:)' because superclass does not have a callable 'init()'}}
    private init?(from: Decoder) {}
}
final class C9: C8 {}
// expected-error@-1 {{class 'C9' has no initializers}}

extension C9: Codable {}
// expected-error@-1 {{type 'C9' does not conform to protocol 'Decodable'}}

struct NotEncodable {}
class C10 {
    var x: NotEncodable = NotEncodable()
    // expected-note@-1 {{cannot automatically synthesize 'Encodable' because 'NotEncodable' does not conform to 'Encodable'}}
}
extension C10: Encodable {}
// expected-error@-1 {{type 'C10' does not conform to protocol 'Encodable'}}
