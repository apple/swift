// RUN: %target-typecheck-verify-swift

class A {}
class B : A {}

let test0 : A.Type = A.self
let test1 : A.Type = B.self
let test2 : B.Type = A.self // expected-error {{cannot convert value of type 'A.Type' to specified type 'B.Type'}}
let test3 : AnyClass = A.self
let test4 : AnyClass = B.self

struct S {}

let test5 : S.Type = S.self
let test6 : AnyClass = S.self // expected-error {{cannot convert value of type 'S.Type' to specified type 'AnyClass' (aka 'any AnyObject.Type')}}

func acceptMeta<T>(_ meta: T.Type) { }
acceptMeta(A) // expected-error {{expected member name or initializer call after type name}}
// expected-note@-1 {{add arguments after the type to construct a value of the type}}
// expected-note@-2 {{use '.self' to reference the type object}}

acceptMeta((A) -> Void) // expected-error {{expected member name or initializer call after type name}}
// expected-note@-1 {{use '.self' to reference the type object}}

func id<T>(_ x: T.Type) -> T.Type { x }

// rdar://62890683: Don't allow arbitrary subtyping for a metatype's instance type.
let _: A?.Type = B.self // expected-error {{cannot convert value of type 'B.Type' to specified type 'A?.Type'}}
let _: A?.Type = id(B.self) // expected-error {{cannot convert value of type 'B.Type' to specified type 'A?.Type'}}
let _: S?.Type = id(S.self) // expected-error {{cannot convert value of type 'S.Type' to specified type 'S?.Type'}}

