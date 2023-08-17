// RUN: %target-typecheck-verify-swift -swift-version 4
// RUN: %target-typecheck-verify-swift -swift-version 4.2

public protocol PublicProtoWithReqs {
  associatedtype Assoc
  func foo()
}

@usableFromInline struct UFIAdopter<T> : PublicProtoWithReqs {}
// expected-warning@-1 {{type alias 'Assoc' must be declared '@usableFromInline' because it matches a requirement in protocol 'PublicProtoWithReqs'; this is an error in Swift 5}} {{none}}
// expected-warning@-2 {{instance method 'foo()' must be declared '@usableFromInline' because it matches a requirement in protocol 'PublicProtoWithReqs'; this is an error in Swift 5}} {{none}}
extension UFIAdopter {
  typealias Assoc = Int
  // expected-note@-1 {{'Assoc' declared here}}
  func foo() {}
  // expected-note@-1 {{'foo()' declared here}}
}

@usableFromInline struct UFIAdopterAllInOne<T> : PublicProtoWithReqs {
  typealias Assoc = Int
  // expected-warning@-1 {{type alias 'Assoc' must be declared '@usableFromInline' because it matches a requirement in protocol 'PublicProtoWithReqs'; this is an error in Swift 5}} {{none}}
  func foo() {}
  // expected-warning@-1 {{instance method 'foo()' must be declared '@usableFromInline' because it matches a requirement in protocol 'PublicProtoWithReqs'; this is an error in Swift 5}} {{none}}
}

internal struct InternalAdopter<T> : PublicProtoWithReqs {}
extension InternalAdopter {
  typealias Assoc = Int // okay
  func foo() {} // okay
}


@usableFromInline protocol UFIProtoWithReqs {
  associatedtype Assoc
  func foo()
}

public struct PublicAdopter<T> : UFIProtoWithReqs {}
// expected-warning@-1 {{type alias 'Assoc' must be declared '@usableFromInline' because it matches a requirement in protocol 'UFIProtoWithReqs'; this is an error in Swift 5}} {{none}}
// expected-warning@-2 {{instance method 'foo()' must be declared '@usableFromInline' because it matches a requirement in protocol 'UFIProtoWithReqs'; this is an error in Swift 5}} {{none}}
extension PublicAdopter {
  typealias Assoc = Int
  // expected-note@-1 {{'Assoc' declared here}}
  func foo() {}
  // expected-note@-1 {{'foo()' declared here}}
}
extension InternalAdopter: UFIProtoWithReqs {} // okay
