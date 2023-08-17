// RUN: %target-typecheck-verify-swift -I %S/Inputs -enable-experimental-cxx-interop

import StdPair

let u = HasMethodThatReturnsUnsafePair()
u.getUnsafePair() // expected-error {{value of type 'HasMethodThatReturnsUnsafePair' has no member 'getUnsafePair'}}
u.getIteratorPair() // expected-error {{value of type 'HasMethodThatReturnsUnsafePair' has no member 'getIteratorPair'}}
