// RUN: %target-typecheck-verify-swift -verify-ignore-unknown -I %S/Inputs -enable-experimental-cxx-interop

import Functions

PrivatelyInherited().constInBase() // expected-error {{value of type 'PrivatelyInherited' has no member 'constInBase'}}
ProtectedInherited().constInBase() // expected-error {{value of type 'ProtectedInherited' has no member 'constInBase'}}


extension Base {
  public func swiftFunc() {}
}

Derived().swiftFunc() // expected-error {{value of type 'Derived' has no member 'swiftFunc'}}

// ok, this calls the derived method.
Derived().sameMethodNameSameSignature()
Derived().sameMethodDifferentSignature(1)
// ok, this is the base class method.
Derived().sameMethodDifferentSignature()
