// RUN: %target-typecheck-verify-swift -verify-ignore-unknown

// Non-Decodable superclasses of synthesized Decodable classes must implement
// init().
class NonDecodableSuper { // expected-note {{cannot automatically synthesize 'init(from:)' because superclass does not have a callable 'init()'}}
  init(_: Int) {}
}

class NonDecodableSub : NonDecodableSuper, Decodable { // expected-error {{type 'NonDecodableSub' does not conform to protocol 'Decodable'}}
}

// Non-Decodable superclasses of synthesized Decodable classes must have
// designated init()'s.
class NonDesignatedNonDecodableSuper {
  convenience init() { // expected-note {{cannot automatically synthesize 'init(from:)' because implementation would need to call 'init()', which is not designated}}
    self.init(42)
  }

  init(_: Int) {}
}

class NonDesignatedNonDecodableSub : NonDesignatedNonDecodableSuper, Decodable { // expected-error {{type 'NonDesignatedNonDecodableSub' does not conform to protocol 'Decodable'}}
}

// Non-Decodable superclasses of synthesized Decodable classes must have an
// accessible init().
class InaccessibleNonDecodableSuper {
  private init() {} // expected-note {{cannot automatically synthesize 'init(from:)' because implementation would need to call 'init()', which is inaccessible due to 'private' protection level}}
}

class InaccessibleNonDecodableSub : InaccessibleNonDecodableSuper, Decodable {}
// expected-error@-1 {{type 'InaccessibleNonDecodableSub' does not conform to protocol 'Decodable'}}
// expected-error@-2 {{class 'InaccessibleNonDecodableSub' has no initializers}}

// Non-Decodable superclasses of synthesized Decodable classes must have a
// non-failable init().
class FailableNonDecodableSuper {
  init?() {} // expected-note {{cannot automatically synthesize 'init(from:)' because implementation would need to call 'init()', which is failable}}
}

class FailableNonDecodableSub : FailableNonDecodableSuper, Decodable { // expected-error {{type 'FailableNonDecodableSub' does not conform to protocol 'Decodable'}}
}

// Subclasses of classes whose Decodable synthesis fails should not inherit
// conformance.
class FailedSynthesisDecodableSuper : Decodable { // expected-error 2{{type 'FailedSynthesisDecodableSuper' does not conform to protocol 'Decodable'}}
  enum CodingKeys : String, CodingKey {
    case nonexistent // expected-note 2{{CodingKey case 'nonexistent' does not match any stored properties}}
  }
}

class FailedSynthesisDecodableSub : FailedSynthesisDecodableSuper { // expected-note {{did you mean 'init'?}}
  func foo() {
    // Decodable should fail to synthesis or be inherited.
    let _ = FailedSynthesisDecodableSub.init(from:) // expected-error {{type 'FailedSynthesisDecodableSub' has no member 'init(from:)'}}
  }
}

// Subclasses of Decodable classes which can't inherit their initializers should
// produce diagnostics.
class DecodableSuper : Decodable {
  var value = 5
}

// expected-note@+1 {{did you mean to override 'init(from:)'?}}{{+1:1-1=\noverride init(from decoder: Decoder) throws {\n    <#code#>\n\}}}
class DecodableSubWithoutInitialValue : DecodableSuper { // expected-error {{class 'DecodableSubWithoutInitialValue' has no initializers}}
// expected-warning{{'required' initializer 'init(from:)' must be provided by subclass of 'DecodableSuper'}}
  var value2: Int // expected-note {{stored property 'value2' without initial value prevents synthesized initializers}}
}

class DecodableSubWithInitialValue : DecodableSuper {
  var value2 = 10
}

// Subclasses of Codable classes which can't inherit their initializers should
// produce diagnostics.
class CodableSuper : Codable {
  var value = 5
}

// expected-note@+1 {{did you mean to override 'init(from:)' and 'encode(to:)'?}}{{+1:1-1=\noverride init(from decoder: Decoder) throws {\n    <#code#>\n\}\n\noverride func encode(to encoder: Encoder) throws {\n    <#code#>\n\}}}
class CodableSubWithoutInitialValue : CodableSuper { // expected-error {{class 'CodableSubWithoutInitialValue' has no initializers}}
  // expected-warning{{'required' initializer 'init(from:)' must be provided by subclass of 'CodableSuper'; this is an error in the Swift 6 language mode}}
  var value2: Int // expected-note {{stored property 'value2' without initial value prevents synthesized initializers}}
}

// We should only mention encode(to:) in the diagnostic if the subclass does not
// override it.
//
// expected-note@+1 {{did you mean to override 'init(from:)'?}}{{+1:1-1=\noverride init(from decoder: Decoder) throws {\n    <#code#>\n\}}}
class EncodableSubWithoutInitialValue : CodableSuper { // expected-error {{class 'EncodableSubWithoutInitialValue' has no initializers}}
  // expected-warning{{'required' initializer 'init(from:)' must be provided by subclass of 'CodableSuper'; this is an error in the Swift 6 language mode}}
  var value2: Int // expected-note {{stored property 'value2' without initial value prevents synthesized initializers}}

  override func encode(to: Encoder) throws {}
}

class CodableSubWithInitialValue : CodableSuper {
  var value2 = 10
}

class GenericCodableSuper<T>: Decodable {}

class GenericCodableSub<T>: GenericCodableSuper<T> {
// expected-error@-1 {{class 'GenericCodableSub' has no initializers}}
// expected-note@-2 {{did you mean to override 'init(from:)'?}}
// expected-warning@-2 {{'required' initializer 'init(from:)' must be provided by subclass of 'GenericCodableSuper<T>'; this is an error in the Swift 6 language mode}}
  var t: T
  // expected-note@-1 {{stored property 't' without initial value prevents synthesized initializers}}
}
