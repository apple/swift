// RUN: %target-typecheck-verify-swift -swift-version 4.2
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-testing
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-library-evolution
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-library-evolution -enable-testing

enum InternalEnum {
  // expected-note@-1 {{type declared here}}
  case apple
  case orange
}

@usableFromInline enum VersionedEnum {
  case apple
  case orange
  case pear(InternalEnum)
  // expected-warning@-1 {{type of enum case in '@usableFromInline' enum should be '@usableFromInline' or public}}
  case persimmon(String)
}

public struct HasInternalSetProperty {
  public internal(set) var x: Int // expected-note {{setter for property 'x' is not '@usableFromInline' or public}}

  @inlinable public mutating func setsX() {
    x = 10 // expected-warning {{setter for property 'x' is internal and should not be referenced from an '@inlinable' function}}
  }
}

@usableFromInline protocol P {
  typealias T = Int
}

extension P {
  @inlinable func f() {
    _ = T.self // typealiases were not checked in Swift 4.2, but P.T inherits @usableFromInline in Swift 4.2 mode
  }
}
