// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: %target-typecheck-verify-swift -debug-generic-signatures -warn-redundant-requirements > %t.dump 2>&1
// RUN: %FileCheck %s < %t.dump

protocol P1 {}
protocol P2 {}
protocol P3 {}
protocol P4: P1 {}
protocol P5: P2 {}
protocol P6: P2 {}

protocol Assoc { associatedtype AT }

func takes_P2<X: P2>(_: X) {}
func takes_P5<X: P5>(_: X) {}

// Skip the first generic signature declcontext dump
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Free

struct Free<T> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Free
// CHECK-NEXT: (normal_conformance type=Free<T> protocol=P2
// CHECK-NEXT:   conforms_to: T P1)
extension Free: P2 where T: P1 {} 
// expected-note@-1 {{requirement from conditional conformance of 'Free<U>' to 'P2'}} 
// expected-note@-2 {{requirement from conditional conformance of 'Free<T>' to 'P2'}}
func free_good<U: P1>(_: U) {
    takes_P2(Free<U>())
}
func free_bad<U>(_: U) {
    takes_P2(Free<U>()) // expected-error{{global function 'takes_P2' requires that 'U' conform to 'P1'}}
}

struct Constrained<T: P1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Constrained
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Constrained
// CHECK-NEXT: (normal_conformance type=Constrained<T> protocol=P2
// CHECK-NEXT:   conforms_to: T P3)
extension Constrained: P2 where T: P3 {} // expected-note {{requirement from conditional conformance of 'Constrained<U>' to 'P2'}}
func constrained_good<U: P1 & P3>(_: U) {
    takes_P2(Constrained<U>())
}
func constrained_bad<U: P1>(_: U) {
  takes_P2(Constrained<U>()) // expected-error{{global function 'takes_P2' requires that 'U' conform to 'P3'}}
}

struct RedundantSame<T: P1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundantSame
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundantSame
// CHECK-NEXT: (normal_conformance type=RedundantSame<T> protocol=P2)
extension RedundantSame: P2 where T: P1 {}
// expected-warning@-1 {{redundant conformance constraint 'T' : 'P1'}}

struct RedundantSuper<T: P4> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundantSuper
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundantSuper
// CHECK-NEXT: (normal_conformance type=RedundantSuper<T> protocol=P2)
extension RedundantSuper: P2 where T: P1 {}
// expected-warning@-1 {{redundant conformance constraint 'T' : 'P1'}}

struct OverlappingSub<T: P1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=OverlappingSub
// CHECK-LABEL: ExtensionDecl line={{.*}} base=OverlappingSub
// CHECK-NEXT: (normal_conformance type=OverlappingSub<T> protocol=P2
// CHECK-NEXT:   conforms_to: T P4)
extension OverlappingSub: P2 where T: P4 {} // expected-note {{requirement from conditional conformance of 'OverlappingSub<U>' to 'P2'}}
// expected-warning@-1 {{redundant conformance constraint 'T' : 'P1'}}
func overlapping_sub_good<U: P4>(_: U) {
    takes_P2(OverlappingSub<U>())
}
func overlapping_sub_bad<U: P1>(_: U) {
  takes_P2(OverlappingSub<U>()) // expected-error{{global function 'takes_P2' requires that 'U' conform to 'P4'}}
}


struct SameType<T> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=SameType
// CHECK-LABEL: ExtensionDecl line={{.*}} base=SameType
// CHECK-NEXT: (normal_conformance type=SameType<T> protocol=P2
// CHECK-NEXT:   same_type: T Int)
extension SameType: P2 where T == Int {}
// expected-note@-1 {{requirement from conditional conformance of 'SameType<U>' to 'P2'}}
// expected-note@-2 {{requirement from conditional conformance of 'SameType<Float>' to 'P2'}}
func same_type_good() {
    takes_P2(SameType<Int>())
}
func same_type_bad<U>(_: U) {
  takes_P2(SameType<U>()) // expected-error{{global function 'takes_P2' requires the types 'U' and 'Int' be equivalent}}
  takes_P2(SameType<Float>()) // expected-error{{global function 'takes_P2' requires the types 'Float' and 'Int' be equivalent}}
}


struct SameTypeGeneric<T, U> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=SameTypeGeneric
// CHECK-LABEL: ExtensionDecl line={{.*}} base=SameTypeGeneric
// CHECK-NEXT: (normal_conformance type=SameTypeGeneric<T, U> protocol=P2
// CHECK-NEXT:   same_type: T U)
extension SameTypeGeneric: P2 where T == U {}
// expected-note@-1 {{requirement from conditional conformance of 'SameTypeGeneric<U, Int>' to 'P2'}}
// expected-note@-2 {{requirement from conditional conformance of 'SameTypeGeneric<Int, Float>' to 'P2'}}
// expected-note@-3 {{requirement from conditional conformance of 'SameTypeGeneric<U, V>' to 'P2'}}
func same_type_generic_good<U, V>(_: U, _: V)
  where U: Assoc, V: Assoc, U.AT == V.AT
{
    takes_P2(SameTypeGeneric<Int, Int>())
    takes_P2(SameTypeGeneric<U, U>())
    takes_P2(SameTypeGeneric<U.AT, V.AT>())
}
func same_type_bad<U, V>(_: U, _: V) {
  takes_P2(SameTypeGeneric<U, V>())
  // expected-error@-1{{global function 'takes_P2' requires the types 'U' and 'V' be equivalent}}
  takes_P2(SameTypeGeneric<U, Int>())
  // expected-error@-1{{global function 'takes_P2' requires the types 'U' and 'Int' be equivalent}}
  takes_P2(SameTypeGeneric<Int, Float>())
  // expected-error@-1{{global function 'takes_P2' requires the types 'Int' and 'Float' be equivalent}}
}


struct Infer<T, U> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Infer
// CHECK-LABEL: ExtensionDecl line={{.*}} base=Infer
// CHECK-NEXT: (normal_conformance type=Infer<T, U> protocol=P2
// CHECK-NEXT:   same_type: T Constrained<U>
// CHECK-NEXT:   conforms_to:  U P1)
extension Infer: P2 where T == Constrained<U> {}
// expected-note@-1 2 {{requirement from conditional conformance of 'Infer<Constrained<U>, V>' to 'P2'}}
func infer_good<U: P1>(_: U) {
    takes_P2(Infer<Constrained<U>, U>())
}
func infer_bad<U: P1, V>(_: U, _: V) {
  takes_P2(Infer<Constrained<U>, V>())
  // expected-error@-1{{global function 'takes_P2' requires the types 'Constrained<U>' and 'Constrained<V>' be equivalent}}
  // expected-error@-2{{global function 'takes_P2' requires that 'V' conform to 'P1'}}
  takes_P2(Infer<Constrained<V>, V>())
  // expected-error@-1{{type 'V' does not conform to protocol 'P1'}}
}

struct InferRedundant<T, U: P1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InferRedundant
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InferRedundant
// CHECK-NEXT: (normal_conformance type=InferRedundant<T, U> protocol=P2
// CHECK-NEXT:   same_type: T Constrained<U>)
extension InferRedundant: P2 where T == Constrained<U> {}
func infer_redundant_good<U: P1>(_: U) {
    takes_P2(InferRedundant<Constrained<U>, U>())
}
func infer_redundant_bad<U: P1, V>(_: U, _: V) {
  takes_P2(InferRedundant<Constrained<U>, V>())
  // expected-error@-1{{type 'V' does not conform to protocol 'P1'}}
  takes_P2(InferRedundant<Constrained<V>, V>())
  // expected-error@-1{{type 'V' does not conform to protocol 'P1'}}
}


class C1 {}
class C2: C1 {}
class C3: C2 {}

struct ClassFree<T> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassFree
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassFree
// CHECK-NEXT: (normal_conformance type=ClassFree<T> protocol=P2
// CHECK-NEXT:   superclass: T C1)
extension ClassFree: P2 where T: C1 {} // expected-note {{requirement from conditional conformance of 'ClassFree<U>' to 'P2'}}
func class_free_good<U: C1>(_: U) {
    takes_P2(ClassFree<U>())
}
func class_free_bad<U>(_: U) {
    takes_P2(ClassFree<U>())
    // expected-error@-1{{global function 'takes_P2' requires that 'U' inherit from 'C1'}}
}

struct ClassMoreSpecific<T: C1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassMoreSpecific
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassMoreSpecific
// CHECK-NEXT: (normal_conformance type=ClassMoreSpecific<T> protocol=P2
// CHECK-NEXT:   superclass: T C3)
extension ClassMoreSpecific: P2 where T: C3 {} // expected-note {{requirement from conditional conformance of 'ClassMoreSpecific<U>' to 'P2'}}
// expected-warning@-1 {{redundant superclass constraint 'T' : 'C1'}}
func class_more_specific_good<U: C3>(_: U) {
    takes_P2(ClassMoreSpecific<U>())
}
func class_more_specific_bad<U: C1>(_: U) {
  takes_P2(ClassMoreSpecific<U>())
  // expected-error@-1{{global function 'takes_P2' requires that 'U' inherit from 'C3'}}
}


struct ClassLessSpecific<T: C3> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassLessSpecific
// CHECK-LABEL: ExtensionDecl line={{.*}} base=ClassLessSpecific
// CHECK-NEXT: (normal_conformance type=ClassLessSpecific<T> protocol=P2)
extension ClassLessSpecific: P2 where T: C1 {}
// expected-warning@-1 {{redundant superclass constraint 'T' : 'C1'}}


// Inherited conformances:
class Base<T> {}
extension Base: P2 where T: C1 {}

class SubclassGood: Base<C1> {}
func subclass_good() {
  takes_P2(SubclassGood())
}
class SubclassBad: Base<Int> {} // expected-note {{requirement from conditional conformance of 'SubclassBad' to 'P2'}}
func subclass_bad() {
  takes_P2(SubclassBad())
  // expected-error@-1{{global function 'takes_P2' requires that 'Int' inherit from 'C1'}}
}

// Inheriting conformances:

struct InheritEqual<T> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritEqual
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritEqual
// CHECK-NEXT:  (normal_conformance type=InheritEqual<T> protocol=P2
// CHECK-NEXT:    conforms_to: T P1)
extension InheritEqual: P2 where T: P1 {} // expected-note {{requirement from conditional conformance of 'InheritEqual<U>' to 'P2'}}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritEqual
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritEqual
// CHECK-NEXT:  (normal_conformance type=InheritEqual<T> protocol=P5
// CHECK-NEXT:    (normal_conformance type=InheritEqual<T> protocol=P2
// CHECK-NEXT:      conforms_to: T P1)
// CHECK-NEXT:    conforms_to: T P1)
extension InheritEqual: P5 where T: P1 {} // expected-note {{requirement from conditional conformance of 'InheritEqual<U>' to 'P5'}}
func inheritequal_good<U: P1>(_: U) {
  takes_P2(InheritEqual<U>())
  takes_P5(InheritEqual<U>())
}
func inheritequal_bad<U>(_: U) {
  takes_P2(InheritEqual<U>()) // expected-error{{global function 'takes_P2' requires that 'U' conform to 'P1'}}
  takes_P5(InheritEqual<U>()) // expected-error{{global function 'takes_P5' requires that 'U' conform to 'P1'}}
}

struct InheritLess<T> {}
extension InheritLess: P2 where T: P1 {}
extension InheritLess: P5 {}
// expected-error@-1 {{type 'InheritLess<T>' does not conform to protocol 'P5'}}
// expected-error@-2 {{type 'T' does not conform to protocol 'P1'}}
// expected-error@-3 {{'P5' requires that 'T' conform to 'P1'}}
// expected-note@-4 {{requirement specified as 'T' : 'P1'}}
// expected-note@-5 {{requirement from conditional conformance of 'InheritLess<T>' to 'P2'}}


struct InheritMore<T> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritMore
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritMore
// CHECK-NEXT:  (normal_conformance type=InheritMore<T> protocol=P2
// CHECK-NEXT:    conforms_to: T P1)
extension InheritMore: P2 where T: P1 {} // expected-note {{requirement from conditional conformance of 'InheritMore<U>' to 'P2'}}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritMore
// CHECK-LABEL: ExtensionDecl line={{.*}} base=InheritMore
// CHECK-NEXT:  (normal_conformance type=InheritMore<T> protocol=P5
// CHECK-NEXT:    (normal_conformance type=InheritMore<T> protocol=P2
// CHECK-NEXT:      conforms_to: T P1)
// CHECK-NEXT:    conforms_to: T P4)
extension InheritMore: P5 where T: P4 {} // expected-note 2 {{requirement from conditional conformance of 'InheritMore<U>' to 'P5'}}
func inheritequal_good_good<U: P4>(_: U) {
  takes_P2(InheritMore<U>())
  takes_P5(InheritMore<U>())
}
func inheritequal_good_bad<U: P1>(_: U) {
  takes_P2(InheritMore<U>())
  takes_P5(InheritMore<U>()) // expected-error{{global function 'takes_P5' requires that 'U' conform to 'P4'}}
}
func inheritequal_bad_bad<U>(_: U) {
  takes_P2(InheritMore<U>()) // expected-error{{global function 'takes_P2' requires that 'U' conform to 'P1'}}
  takes_P5(InheritMore<U>()) // expected-error{{global function 'takes_P5' requires that 'U' conform to 'P4'}}
}

struct InheritImplicitOne<T> {}
// This shouldn't give anything implicit since we disallow implication for
// conditional conformances (in many cases, the implied bounds are
// incorrect/insufficiently general).
extension InheritImplicitOne: P5 where T: P1 {}
// expected-error@-1{{conditional conformance of type 'InheritImplicitOne<T>' to protocol 'P5' does not imply conformance to inherited protocol 'P2'}}
// expected-note@-2{{did you mean to explicitly state the conformance like 'extension InheritImplicitOne: P2 where ...'?}}

struct InheritImplicitTwo<T> {}
// Even if we relax the rule about implication, this double-up should still be
// an error, because either conformance could imply InheritImplicitTwo: P2.
extension InheritImplicitTwo: P5 where T: P1 {}
// expected-error@-1{{conditional conformance of type 'InheritImplicitTwo<T>' to protocol 'P5' does not imply conformance to inherited protocol 'P2'}}
// expected-note@-2{{did you mean to explicitly state the conformance like 'extension InheritImplicitTwo: P2 where ...'?}}
extension InheritImplicitTwo: P6 where T: P1 {}

// However, if there's a non-conditional conformance that implies something, we
// can imply from that one.
struct InheritImplicitGood1<T> {}
extension InheritImplicitGood1: P5 {}
extension InheritImplicitGood1: P6 where T: P1 {}

func inheritimplicitgood1<T>(_ : T) {
    takes_P2(InheritImplicitGood1<T>()) // unconstrained!
    takes_P2(InheritImplicitGood1<Int>())
}
struct InheritImplicitGood2<T> {}
extension InheritImplicitGood2: P6 where T: P1 {}
extension InheritImplicitGood2: P5 {}

func inheritimplicitgood2<T>(_: T) {
    takes_P2(InheritImplicitGood2<T>()) // unconstrained!
    takes_P2(InheritImplicitGood2<Int>())

}
struct InheritImplicitGood3<T>: P5 {}
extension InheritImplicitGood3: P6 where T: P1 {}

func inheritimplicitgood3<T>(_: T) {
    takes_P2(InheritImplicitGood3<T>()) // unconstrained!
    takes_P2(InheritImplicitGood3<Int>())
}

// "Multiple conformances" from SE0143

struct TwoConformances<T> {}
extension TwoConformances: P2 where T: P1 {}
// expected-note@-1{{'TwoConformances<T>' declares conformance to protocol 'P2' here}}
extension TwoConformances: P2 where T: P3 {}
// expected-error@-1{{conflicting conformance of 'TwoConformances<T>' to protocol 'P2'; there cannot be more than one conformance, even with different conditional bounds}}

struct TwoDisjointConformances<T> {}
extension TwoDisjointConformances: P2 where T == Int {}
// expected-note@-1{{'TwoDisjointConformances<T>' declares conformance to protocol 'P2' here}}
extension TwoDisjointConformances: P2 where T == String {}
// expected-error@-1{{conflicting conformance of 'TwoDisjointConformances<T>' to protocol 'P2'; there cannot be more than one conformance, even with different conditional bounds}}


// FIXME: these cases should be equivalent (and both with the same output as the
// first), but the second one choses T as the representative of the
// equivalence class containing both T and U in the extension's generic
// signature, meaning the stored conditional requirement is T: P1, which isn't
// true in the original type's generic signature.
struct RedundancyOrderDependenceGood<T: P1, U> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundancyOrderDependenceGood
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundancyOrderDependenceGood
// CHECK-NEXT: (normal_conformance type=RedundancyOrderDependenceGood<T, U> protocol=P2
// CHECK-NEXT:   same_type: T U)
extension RedundancyOrderDependenceGood: P2 where U: P1, T == U {}
// expected-warning@-1 {{redundant conformance constraint 'U' : 'P1'}}

struct RedundancyOrderDependenceBad<T, U: P1> {}
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundancyOrderDependenceBad
// CHECK-LABEL: ExtensionDecl line={{.*}} base=RedundancyOrderDependenceBad
// CHECK-NEXT: (normal_conformance type=RedundancyOrderDependenceBad<T, U> protocol=P2
// CHECK-NEXT:   conforms_to: T P1
// CHECK-NEXT:   same_type: T U)
extension RedundancyOrderDependenceBad: P2 where T: P1, T == U {} // expected-warning {{redundant conformance constraint 'U' : 'P1'}}

// Checking of conditional requirements for existential conversions.
func existential_good<T: P1>(_: T.Type) {
  _ = Free<T>() as P2
}

func existential_bad<T>(_: T.Type) {
  _ = Free<T>() as P2 // expected-error{{protocol 'P2' requires that 'T' conform to 'P1'}}
}

// rdar://problem/35837054
protocol P7 { }

protocol P8 {
  associatedtype A
}

struct X0 { }

struct X1 { }

extension X1: P8 {
  typealias A = X0
}

struct X2<T> { }

extension X2: P7 where T: P8, T.A: P7 { } // expected-note {{requirement from conditional conformance of 'X2<X1>' to 'P7'}}

func takesF7<T: P7>(_: T) { }
func passesConditionallyNotF7(x21: X2<X1>) {
  takesF7(x21) // expected-error{{global function 'takesF7' requires that 'X1.A' (aka 'X0') conform to 'P7'}}
}

// https://github.com/apple/swift/issues/49538
public struct S_49538<T, U> {}
extension S_49538: Sequence where T == Int {
    public typealias Element = Float
    public typealias Iterator = IndexingIterator<[Float]>
    public func makeIterator() -> Iterator { fatalError() }
}

// https://github.com/apple/swift/issues/50852
protocol ElementProtocol {
  associatedtype BaseElement: BaseElementProtocol = Self
}
protocol BaseElementProtocol: ElementProtocol where BaseElement == Self {}
protocol ArrayProtocol {
  associatedtype Element: ElementProtocol
}
protocol NestedArrayProtocol: ArrayProtocol where Element: ArrayProtocol, Element.Element.BaseElement == Element.BaseElement {
  associatedtype BaseElement = Element.BaseElement
}
extension Array: ArrayProtocol where Element: ElementProtocol {}
extension Array: NestedArrayProtocol where Element: ElementProtocol, Element: ArrayProtocol, Element.Element.BaseElement == Element.BaseElement {
  // with the typealias uncommented you do not get a crash.
  // typealias BaseElement = Element.BaseElement
}

// https://github.com/apple/swift/issues/50865

struct Foo<Bar> {}

protocol P {
  associatedtype A
  var foo: Foo<A> { get }
}

extension Foo: P where Bar: P {
  var foo: Foo { return self }
}

// rdar://problem/47871590

extension BinaryInteger {
  var foo: Self {
    return self <= 1
            ? 1
            : (2...self).reduce(1, *)
            // expected-error@-1 {{referencing instance method 'reduce' on 'ClosedRange' requires that 'Self.Stride' conform to 'SignedInteger'}}
  }
}

// https://github.com/apple/swift/issues/53382

protocol P_53382 {}
struct S_53382<T> {}
extension S_53382: P_53382 where T: P_53382 {} // expected-note {{requirement from conditional conformance of 'S_53382<String>' to 'P_53382'}}
	
func f1_53382(_ fn: (S_53382<String>) -> Void) {}
func f2_53382(_ fn: (P_53382) -> Void) {
  f1_53382(fn) // expected-error {{global function 'f1_53382' requires that 'String' conform to 'P_53382'}}
}
