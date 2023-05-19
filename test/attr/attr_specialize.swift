// RUN: %target-typecheck-verify-swift
// RUN: %target-swift-ide-test -print-ast-typechecked -source-filename=%s -disable-objc-attr-requires-foundation-module -define-availability 'SwiftStdlib 5.1:macOS 10.15, iOS 13.0, watchOS 6.0, tvOS 13.0' | %FileCheck %s

struct S<T> {}

public protocol P {
}
extension Int: P {
}

public protocol ProtocolWithDep {
  associatedtype Element
  func getElement() -> Element
}

public class C1 {
}

class Base {}
class Sub : Base {}
class NonSub {}

// Specialize freestanding functions with the correct number of concrete types.
// ----------------------------------------------------------------------------

// CHECK: @_specialize(exported: false, kind: full, where T == Int)
@_specialize(where T == Int)
// CHECK: @_specialize(exported: false, kind: full, where T == S<Int>)
@_specialize(where T == S<Int>)
@_specialize(where T == Int, U == Int) // expected-error{{cannot find type 'U' in scope}},
@_specialize(where T == T1) // expected-error{{cannot find type 'T1' in scope}}
public func oneGenericParam<T>(_ t: T) -> T {
  return t
}

// CHECK: @_specialize(exported: false, kind: full, where T == Int, U == Int)
@_specialize(where T == Int, U == Int)
@_specialize(where T == Int) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note{{missing constraint for 'U' in '_specialize' attribute}}
public func twoGenericParams<T, U>(_ t: T, u: U) -> (T, U) {
  return (t, u)
}

@_specialize(where T == Int) // expected-error{{trailing 'where' clause in '_specialize' attribute of non-generic function 'nonGenericParam(x:)'}}
func nonGenericParam(x: Int) {}

// Specialize contextual types.
// ----------------------------

class G<T> {
  // CHECK: @_specialize(exported: false, kind: full, where T == Int)
  @_specialize(where T == Int)
  @_specialize(where T == T) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
  // expected-note@-1 {{missing constraint for 'T' in '_specialize' attribute}}
  @_specialize(where T == S<T>)
  // expected-error@-1 {{cannot build rewrite system for generic signature; concrete nesting limit exceeded}}
  // expected-note@-2 {{failed rewrite rule is τ_0_0.[concrete: S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<S<τ_0_0>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>] => τ_0_0}}
  // expected-error@-3 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
  // expected-note@-4 {{missing constraint for 'T' in '_specialize' attribute}}
  @_specialize(where T == Int, U == Int) // expected-error{{cannot find type 'U' in scope}}

  func noGenericParams() {}

  // CHECK: @_specialize(exported: false, kind: full, where T == Int, U == Float)
  @_specialize(where T == Int, U == Float)
  // CHECK: @_specialize(exported: false, kind: full, where T == Int, U == S<Int>)
  @_specialize(where T == Int, U == S<Int>)
  @_specialize(where T == Int) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note {{missing constraint for 'U' in '_specialize' attribute}}
  func oneGenericParam<U>(_ t: T, u: U) -> (U, T) {
    return (u, t)
  }
}

// Specialize with requirements.
// -----------------------------

protocol Thing {}

struct AThing : Thing {}

// CHECK: @_specialize(exported: false, kind: full, where T == AThing)
@_specialize(where T == AThing)
@_specialize(where T == Int) // expected-error{{no type for 'T' can satisfy both 'T == Int' and 'T : Thing'}}
// expected-error@-1 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-2 {{missing constraint for 'T' in '_specialize' attribute}}

func oneRequirement<T : Thing>(_ t: T) {}

protocol HasElt {
  associatedtype Element
}
struct IntElement : HasElt {
  typealias Element = Int
}
struct FloatElement : HasElt {
  typealias Element = Float
}
@_specialize(where T == FloatElement)
@_specialize(where T == IntElement) // expected-error{{generic signature requires types 'IntElement.Element' (aka 'Int') and 'Float' to be the same}}
func sameTypeRequirement<T : HasElt>(_ t: T) where T.Element == Float {}

@_specialize(where T == Sub)
@_specialize(where T == NonSub) // expected-error{{no type for 'T' can satisfy both 'T : NonSub' and 'T : Base'}}
func superTypeRequirement<T : Base>(_ t: T) {}

@_specialize(where X:_Trivial(8), Y == Int) // expected-error{{trailing 'where' clause in '_specialize' attribute of non-generic function 'requirementOnNonGenericFunction(x:y:)'}}
public func requirementOnNonGenericFunction(x: Int, y: Int) {
}

@_specialize(where Y == Int) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note{{missing constraint for 'X' in '_specialize' attribute}}
public func missingRequirement<X:P, Y>(x: X, y: Y) {
}

@_specialize(where) // expected-error{{expected type}}
@_specialize() // expected-error{{expected a parameter label or a where clause in '_specialize' attribute}} expected-error{{expected declaration}}
public func funcWithEmptySpecializeAttr<X: P, Y>(x: X, y: Y) {
}


@_specialize(where X:_Trivial(8), Y:_Trivial(32), Z == Int) // expected-error{{cannot find type 'Z' in scope}}
@_specialize(where X:_Trivial(8), Y:_Trivial(32, 4))
@_specialize(where X == Int) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note{{missing constraint for 'Y' in '_specialize' attribute}}
@_specialize(where Y:_Trivial(32)) // expected-error {{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note{{missing constraint for 'X' in '_specialize' attribute}}
@_specialize(where Y: P) // expected-error{{only same-type and layout requirements are supported by '_specialize' attribute}}
@_specialize(where Y: MyClass) // expected-error{{cannot find type 'MyClass' in scope}} expected-error{{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 2)}} expected-note{{missing constraint for 'X' in '_specialize' attribute}} expected-note{{missing constraint for 'Y' in '_specialize' attribute}}
@_specialize(where X:_Trivial(8), Y == Int)
@_specialize(where X == Int, Y == Int)
@_specialize(where X == Int, X == Int) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 1, but expected 2)}} expected-note{{missing constraint for 'Y' in '_specialize' attribute}}
@_specialize(where Y:_Trivial(32), X == Float)
@_specialize(where X1 == Int, Y1 == Int) // expected-error{{cannot find type 'X1' in scope}} expected-error{{cannot find type 'Y1' in scope}} expected-error{{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 2)}} expected-note{{missing constraint for 'X' in '_specialize' attribute}} expected-note{{missing constraint for 'Y' in '_specialize' attribute}}
public func funcWithTwoGenericParameters<X, Y>(x: X, y: Y) {
}

@_specialize(where X == Int, Y == Int)
@_specialize(exported: true, where X == Int, Y == Int)
@_specialize(exported: false, where X == Int, Y == Int)
@_specialize(exported: false where X == Int, Y == Int) // expected-error{{missing ',' in '_specialize' attribute}}
@_specialize(exported: yes, where X == Int, Y == Int) // expected-error{{expected a boolean true or false value in '_specialize' attribute}}
@_specialize(exported: , where X == Int, Y == Int) // expected-error{{expected a boolean true or false value in '_specialize' attribute}}

@_specialize(kind: partial, where X == Int, Y == Int)
@_specialize(kind: partial, where X == Int)
@_specialize(kind: full, where X == Int, Y == Int)
@_specialize(kind: any, where X == Int, Y == Int) // expected-error{{expected 'partial' or 'full' as values of the 'kind' parameter in '_specialize' attribute}}
@_specialize(kind: false, where X == Int, Y == Int) // expected-error{{expected 'partial' or 'full' as values of the 'kind' parameter in '_specialize' attribute}}
@_specialize(kind: partial where X == Int, Y == Int) // expected-error{{missing ',' in '_specialize' attribute}}
@_specialize(kind: partial, where X == Int, Y == Int)
@_specialize(kind: , where X == Int, Y == Int)

@_specialize(exported: true, kind: partial, where X == Int, Y == Int)
@_specialize(exported: true, exported: true, where X == Int, Y == Int) // expected-error{{parameter 'exported' was already defined in '_specialize' attribute}}
@_specialize(kind: partial, exported: true, where X == Int, Y == Int)
@_specialize(kind: partial, kind: partial, where X == Int, Y == Int) // expected-error{{parameter 'kind' was already defined in '_specialize' attribute}}

@_specialize(where X == Int, Y == Int, exported: true, kind: partial) // expected-error{{cannot find type 'exported' in scope}} expected-error{{cannot find type 'kind' in scope}} expected-error{{cannot find type 'partial' in scope}} expected-error{{expected type}}
public func anotherFuncWithTwoGenericParameters<X: P, Y>(x: X, y: Y) {
}

@_specialize(where T: P) // expected-error{{only same-type and layout requirements are supported by '_specialize' attribute}}
@_specialize(where T: Int) // expected-error{{type 'T' constrained to non-protocol, non-class type 'Int'}} expected-note {{use 'T == Int' to require 'T' to be 'Int'}}
// expected-error@-1 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-2 {{missing constraint for 'T' in '_specialize' attribute}}

@_specialize(where T: S1) // expected-error{{type 'T' constrained to non-protocol, non-class type 'S1'}} expected-note {{use 'T == S1' to require 'T' to be 'S1'}}
// expected-error@-1 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-2 {{missing constraint for 'T' in '_specialize' attribute}}
@_specialize(where T: C1) // expected-error{{only same-type and layout requirements are supported by '_specialize' attribute}}
@_specialize(where Int: P) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}} expected-note{{missing constraint for 'T' in '_specialize' attribute}}
func funcWithForbiddenSpecializeRequirement<T>(_ t: T) {
}

@_specialize(where T: _Trivial(32), T: _Trivial(64), T: _Trivial, T: _RefCountedObject)
// expected-error@-1{{no type for 'T' can satisfy both 'T : _RefCountedObject' and 'T : _Trivial(32)'}}
// expected-error@-2{{no type for 'T' can satisfy both 'T : _Trivial(64)' and 'T : _Trivial(32)'}}
// expected-error@-3 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-4 {{missing constraint for 'T' in '_specialize' attribute}}
@_specialize(where T: _Trivial, T: _Trivial(64))
@_specialize(where T: _RefCountedObject, T: _NativeRefCountedObject)
@_specialize(where Array<T> == Int) // expected-error{{generic signature requires types 'Array<T>' and 'Int' to be the same}}
// expected-error@-1 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-2 {{missing constraint for 'T' in '_specialize' attribute}}
@_specialize(where T.Element == Int) // expected-error{{only requirements on generic parameters are supported by '_specialize' attribute}}
public func funcWithComplexSpecializeRequirements<T: ProtocolWithDep>(t: T) -> Int {
  return 55555
}

public protocol Proto: class {
}

@_specialize(where T: _RefCountedObject)
// expected-error@-1 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-2 {{missing constraint for 'T' in '_specialize' attribute}}
@_specialize(where T: _Trivial)
// expected-error@-1{{no type for 'T' can satisfy both 'T : _NativeClass' and 'T : _Trivial'}}
// expected-error@-2 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-3 {{missing constraint for 'T' in '_specialize' attribute}}
@_specialize(where T: _Trivial(64))
// expected-error@-1{{no type for 'T' can satisfy both 'T : _NativeClass' and 'T : _Trivial(64)'}}
// expected-error@-2 {{too few generic parameters are specified in '_specialize' attribute (got 0, but expected 1)}}
// expected-note@-3 {{missing constraint for 'T' in '_specialize' attribute}}
public func funcWithABaseClassRequirement<T>(t: T) -> Int where T: C1 {
  return 44444
}

public struct S1 {
}

@_specialize(exported: false, where T == Int64)
public func simpleGeneric<T>(t: T) -> T {
  return t
}


@_specialize(exported: true, where S: _Trivial(64))
// Check that any bitsize size is OK, not only powers of 8.
@_specialize(where S: _Trivial(60))
@_specialize(exported: true, where S: _RefCountedObject)
@inline(never)
public func copyValue<S>(_ t: S, s: inout S) -> Int64 where S: P{
  return 1
}

@_specialize(exported: true, where S: _Trivial)
@_specialize(exported: true, where S: _Trivial(64))
@_specialize(exported: true, where S: _Trivial(32))
@_specialize(exported: true, where S: _RefCountedObject)
@_specialize(exported: true, where S: _NativeRefCountedObject)
@_specialize(exported: true, where S: _Class)
@_specialize(exported: true, where S: _NativeClass)
@inline(never)
public func copyValueAndReturn<S>(_ t: S, s: inout S) -> S where S: P{
  return s
}

struct OuterStruct<S> {
  struct MyStruct<T> {
    @_specialize(where T == Int, U == Float) // expected-error{{too few generic parameters are specified in '_specialize' attribute (got 2, but expected 3)}} expected-note{{missing constraint for 'S' in '_specialize' attribute}}
    public func foo<U>(u : U) {
    }

    @_specialize(where T == Int, U == Float, S == Int)
    public func bar<U>(u : U) {
    }
  }
}

// Check _TrivialAtMostN constraints.
@_specialize(exported: true, where S: _TrivialAtMost(64))
@inline(never)
public func copy2<S>(_ t: S, s: inout S) -> S where S: P{
  return s
}

// Check missing alignment.
@_specialize(where S: _Trivial(64, )) // expected-error{{expected non-negative alignment to be specified in layout constraint}}
// Check non-numeric size.
@_specialize(where S: _Trivial(Int)) // expected-error{{expected non-negative size to be specified in layout constraint}}
// Check non-numeric alignment.
@_specialize(where S: _Trivial(64, X)) // expected-error{{expected non-negative alignment to be specified in layout constraint}}
@inline(never)
public func copy3<S>(_ s: S) -> S {
  return s
}

public func funcWithWhereClause<T>(t: T) where T:P, T: _Trivial(64) { // expected-error{{layout constraints are only allowed inside '_specialize' attributes}}
}

// rdar://problem/29333056
public protocol P1 {
  associatedtype DP1
  associatedtype DP11
}

public protocol P2 {
  associatedtype DP2 : P1
}

public struct H<T> {
}

public struct MyStruct3 : P1 {
  public typealias DP1 = Int
  public typealias DP11 = H<Int>
}

public struct MyStruct4 : P2 {
  public typealias DP2 = MyStruct3
}

@_specialize(where T==MyStruct4)
public func foo<T: P2>(_ t: T) where T.DP2.DP11 == H<T.DP2.DP1> {
}

public func targetFun<T>(_ t: T) {}

@_specialize(exported: true, target: targetFun(_:), where T == Int)
public func specifyTargetFunc<T>(_ t: T) {
}

public struct Container {
  public func targetFun<T>(_ t: T) {}
}

extension Container {
  @_specialize(exported: true, target: targetFun(_:), where T == Int)
  public func specifyTargetFunc<T>(_ t: T) { }

  @_specialize(exported: true, target: targetFun2(_:), where T == Int) // expected-error{{target function 'targetFun2' could not be found}}
  public func specifyTargetFunc2<T>(_ t: T) { }
}

// Make sure we don't complain that 'E' is not explicitly specialized here.
// E becomes concrete via the combination of 'S == Set<String>' and
// 'E == S.Element'.
@_specialize(where S == Set<String>)
public func takesSequenceAndElement<S, E>(_: S, _: E)
  where S : Sequence, E == S.Element {}

// CHECK: @_specialize(exported: true, kind: full, availability: macOS 11, iOS 13, *; where T == Int)
// CHECK: public func testAvailability<T>(_ t: T)
@_specialize(exported: true, availability: macOS 11, iOS 13, *; where T == Int)
public func testAvailability<T>(_ t: T) {}

// CHECK: @_specialize(exported: true, kind: full, availability: macOS, introduced: 11; where T == Int)
// CHECK: public func testAvailability2<T>(_ t: T)
@_specialize(exported: true, availability: macOS 11, *; where T == Int)
public func testAvailability2<T>(_ t: T) {}

// CHECK: @_specialize(exported: true, kind: full, availability: macOS, introduced: 11; where T == Int)
// CHECK: public func testAvailability3<T>(_ t: T)
@_specialize(exported: true, availability: macOS, introduced: 11; where T == Int)
public func testAvailability3<T>(_ t: T) {}

// CHECK: @_specialize(exported: true, kind: full, availability: macOS 10.15, iOS 13.0, watchOS 6.0, tvOS 13.0, *; where T == Int)
// CHECK: public func testAvailability4<T>(_ t: T)
@_specialize(exported: true, availability: SwiftStdlib 5.1, *; where T == Int)
public func testAvailability4<T>(_ t: T) {}

public struct ExpectedElement {
  @inline(never)
  public func hello() {}
}

public struct ConformerElement {}

public struct Conformer : ProtocolWithDep {
  public typealias Element = ConformerElement
  public func getElement() -> ConformerElement { return ConformerElement() }
}

@inline(never)
@_specialize(where T == Conformer) // expected-error{{generic signature requires types 'Conformer.Element' (aka 'ConformerElement') and 'ExpectedElement' to be the same}}
public func foo<T : ProtocolWithDep>(_ t: T) where T.Element == ExpectedElement {
  t.getElement().hello()
}

@_specialize(where T == Conformer) // expected-error{{generic signature requires types 'Conformer.Element' (aka 'ConformerElement') and 'ExpectedElement' to be the same}}
public func bar<T : ProtocolWithDep>(_ t: T) where T.Element == ExpectedElement {
  foo(t)
}
