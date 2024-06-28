// Tests that a C++ class can conform to a Swift protocol.

// RUN: %target-typecheck-verify-swift -I %S/Inputs -enable-experimental-cxx-interop
// RUN: %target-typecheck-verify-swift -I %S/Inputs -D VIRTUAL_METHODS -cxx-interoperability-mode=swift-5.9
// RUN: %target-typecheck-verify-swift -I %S/Inputs -D VIRTUAL_METHODS -cxx-interoperability-mode=swift-6
// RUN: %target-typecheck-verify-swift -I %S/Inputs -D VIRTUAL_METHODS -cxx-interoperability-mode=upcoming-swift

import ProtocolConformance

protocol HasReturn42 {
  mutating func return42() -> CInt // expected-note {{requires function 'return42()'}}
}

extension ConformsToProtocol : HasReturn42 {}

#if VIRTUAL_METHODS
extension HasVirtualMethod : HasReturn42 {}
#endif

extension DoesNotConformToProtocol : HasReturn42 {} // expected-error {{'DoesNotConformToProtocol' does not conform to protocol}}


protocol HasReturnNullable {
  mutating func returnPointer() -> UnsafePointer<Int32>?
}

// HasReturnNullable's returnNullable returns an implicitly unwrapped optional:
//   mutating func returnPointer() -> UnsafePointer<Int32>!
extension ReturnsNullableValue: HasReturnNullable {}

protocol HasReturnNonNull {
  mutating func returnPointer() -> UnsafePointer<Int32>
}

extension ReturnsNonNullValue: HasReturnNonNull {}


protocol Invertible {
  static prefix func !(obj: Self) -> Self
}

extension HasOperatorExclaim: Invertible {}

extension HasOperatorEqualEqual: @retroactive Equatable {}


protocol HasOperatorPlusEqualProtocol {
  static func +=(lhs: inout Self, x: Int32)
}

extension HasOperatorPlusEqualInt : HasOperatorPlusEqualProtocol {}

protocol HasOperatorCall {
  func callAsFunction(_ x: Int32) -> Int32
}

extension HasStaticOperatorCall : HasOperatorCall {}