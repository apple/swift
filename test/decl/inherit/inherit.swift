// RUN: %target-typecheck-verify-swift -swift-version 5

public class A { }
public protocol P { }
public protocol P1 { }

// Duplicate inheritance
class B : A, A { } // expected-error{{duplicate inheritance from 'A'}}{{12-15=}}

// Duplicate inheritance from protocol
class B2 : P, P { }
// expected-note@-1 {{'B2' declares conformance to protocol 'P' here}}
// expected-error@-2 {{redundant conformance of 'B2' to protocol 'P'}}

// Multiple inheritance
class C : B, A { } // expected-error{{multiple inheritance from classes 'B' and 'A'}}

// Superclass in the wrong position
class D : P, A { } // expected-error{{superclass 'A' must appear first in the inheritance clause}}{{12-15=}}{{11-11=A, }}

// SR-8160
class D1 : Any, A { } // expected-error{{superclass 'A' must appear first in the inheritance clause}}{{15-18=}}{{12-12=A, }}

class D2 : P & P1, A { } // expected-error{{superclass 'A' must appear first in the inheritance clause}}{{18-21=}}{{12-12=A, }}

@usableFromInline
class D3 : Any, A { } // expected-error{{superclass 'A' must appear first in the inheritance clause}}{{15-18=}}{{12-12=A, }}

@usableFromInline
class D4 : P & P1, A { } // expected-error{{superclass 'A' must appear first in the inheritance clause}}{{18-21=}}{{12-12=A, }}

// Struct inheriting a class
struct S : A { } // expected-error{{struct type cannot inherit from non-protocol type 'A'}}

// Protocol inheriting a class
protocol Q : A { }

// Extension inheriting a class
extension C : A { } // expected-error{{extension type cannot inherit from non-protocol type 'A'}}

// Keywords in inheritance clauses
struct S2 : struct { } // expected-error{{expected type}}

// Protocol composition in inheritance clauses
struct S3 : P, P & Q { } // expected-error {{redundant conformance of 'S3' to protocol 'P'}}
                         // expected-error @-1 {{non-class type 'S3' cannot conform to class protocol 'Q'}}
                         // expected-note @-2 {{'S3' declares conformance to protocol 'P' here}}
struct S4 : P, P { }
// expected-error@-1{{redundant conformance of 'S4' to protocol 'P'}}
// expected-note@-2{{'S4' declares conformance to protocol 'P' here}}
struct S6 : P & { }      // expected-error {{expected identifier for type name}}
struct S7 : protocol<P, Q> { }  // expected-error {{'protocol<...>' composition syntax has been removed; join the protocols using '&'}}
                                // expected-error @-1 {{non-class type 'S7' cannot conform to class protocol 'Q'}}

class GenericBase<T> {}

class GenericSub<T> : GenericBase<T> {} // okay

class InheritGenericParam<T> : T {} // expected-error {{generic class type cannot inherit from non-protocol, non-class type 'T'}}
class InheritBody : T { // expected-error {{cannot find type 'T' in scope}}
  typealias T = A
}
class InheritBodyBad : fn { // expected-error {{cannot find type 'fn' in scope}}
  func fn() {}
}
