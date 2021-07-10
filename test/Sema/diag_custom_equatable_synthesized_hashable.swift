// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/NormalLibrary.swiftmodule %S/Inputs/external_equatable_conformance.swift

// RUN: %target-typecheck-verify-swift -I %t

import NormalLibrary

// All conformance synthesized no warning emitted
struct NoCustomEquatable: Hashable {
  let a:Int
}

// Equatable implemented and Hashable synthesized raise a warning
enum CustomEquatable: Hashable {
  case a

  static func == (lhs: CustomEquatable, rhs: CustomEquatable) -> Bool {
    // expected-warning@-1 {{automatically generated 'Hashable' implementation for type 'CustomEquatable' may not match the behavior of custom '==' operator}}
    // expected-note@-2 {{add a custom 'hash(into:)' method}}
    return true
  }
}

// All conformance implemented no warning emitted
enum CustomHashable: Hashable {
  case a

  func hash(into hasher: inout Hasher) {}

  static func == (lhs: CustomHashable, rhs: CustomHashable) -> Bool {
    return true
  }
}

struct ExtendedConformance: Hashable {
  let a:Int
}

extension ExtendedConformance {
  static func == (lhs: ExtendedConformance, rhs: ExtendedConformance) -> Bool {
    // expected-warning@-1 {{automatically generated 'Hashable' implementation for type 'ExtendedConformance' may not match the behavior of custom '==' operator}}
    // expected-note@-2 {{add a custom 'hash(into:)' method}}
    return true
  }
}

enum ImportedConformance: ImplicitEquatable {
  case x = 1
}

enum ImportedExplicitConformance: ExplicitEquatable {
  case x = 1
}

protocol DefaultedImplementationProtocol: Equatable {}
extension DefaultedImplementationProtocol {
  static func == (lhs: Self, rhs: Self) -> Bool { true }
  // expected-warning@-1 {{automatically generated 'Hashable' implementation for type 'DefaultedImplementation' may not match the behavior of custom '==' operator}}
  // expected-note@-2 {{add a custom 'hash(into:)' method}}
}

struct DefaultedImplementation: DefaultedImplementationProtocol, Hashable {
  let a: Int
}

protocol ConditionalImplementationProtocol {}
extension ConditionalImplementationProtocol where Self: Equatable {
  static func == (lhs: Self, rhs: Self) -> Bool { true }
  // expected-warning@-1 {{automatically generated 'Hashable' implementation for type 'ConditionalImplementation' may not match the behavior of custom '==' operator}}
  // expected-note@-2 {{add a custom 'hash(into:)' method}}
}

struct ConditionalImplementation: ConditionalImplementationProtocol, Hashable {
  let a: Int
}

protocol ConditionalHashableImplementationProtocol {}
extension ConditionalHashableImplementationProtocol where Self: Equatable {
  static func == (lhs: Self, rhs: Self) -> Bool { true }
  // expected-warning@-1 {{automatically generated 'Hashable' implementation for type 'ConditionalHashableImplementation' may not match the behavior of custom '==' operator}}
  // expected-note@-2 {{add a custom 'hash(into:)' method}}
}

extension ConditionalHashableImplementationProtocol where Self: Hashable {
  func hash(into hasher: Hasher) {}
}

struct ConditionalHashableImplementation: ConditionalHashableImplementationProtocol, Hashable {
  let a: Int
}
