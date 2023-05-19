// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: %target-swift-frontend -typecheck -debug-generic-signatures %s 2>&1 | %FileCheck %s

protocol P1 {
  associatedtype T
}

protocol P2 {
  associatedtype T : P1
}

struct S1<A, B, C> : P2
    where A : P1, C : P2, B == A.T.T, C.T == A.T {
  typealias T = C.T
}

struct S2<D : P1> : P2 {
  typealias T = D
}

// Make sure that we can resolve the nested type A.T
// in the same-type requirement 'A.T == B.T'.
//
// A is concrete, and S1<C, E, S2<D>>.T resolves to
// S2<D>.T, which is D. The typealias S1.T has a
// structural type 'C.T'; since C is concrete, we
// have to handle the case of an unresolved member
// type with a concrete base.
struct UnresolvedWithConcreteBase<A, B> {
  // CHECK-LABEL: Generic signature: <A, B, C, D, E where A == S1<C, E, S2<D>>, B : P2, C : P1, D == B.[P2]T, E == D.[P1]T, B.[P2]T == C.[P1]T>
  init<C, D, E>(_: C)
    where A == S1<C, E, S2<D>>,
          B : P2,
          A.T == B.T,
          C : P1,
          D == C.T,
          E == D.T { }
}

// Make sure that we drop the conformance requirement
// 'A : P2' and rebuild the generic signature with the
// correct same-type requirements.
//
// The original test case in the bug report (correctly)
// produces two warnings about redundant requirements.
struct OriginalExampleWithWarning<A, B> where A : P2, B : P2, A.T == B.T {
  // CHECK-LABEL: Generic signature: <A, B, C, D, E where A == S1<C, E, S2<D>>, B : P2, C : P1, D == B.[P2]T, E == D.[P1]T, B.[P2]T == C.[P1]T>
  init<C, D, E>(_: C) // expected-warning {{redundant conformance constraint 'S1<C, C.T.T, S2<C.T>>' : 'P2'}}
    where C : P1,
          D : P1, // expected-warning {{redundant conformance constraint 'D' : 'P1'}}
          C.T : P1, // expected-warning {{redundant conformance constraint 'C.T' : 'P1'}}
          A == S1<C, C.T.T, S2<C.T>>,
          C.T == D,
          E == D.T { }
}

// Same as above but without the warnings.
struct OriginalExampleWithoutWarning<A, B> where A : P2, B : P2, A.T == B.T {
  // CHECK-LABEL: Generic signature: <A, B, C, D, E where A == S1<C, E, S2<D>>, B : P2, C : P1, D == B.[P2]T, E == D.[P1]T, B.[P2]T == C.[P1]T>
  init<C, D, E>(_: C) // expected-warning {{redundant conformance constraint 'S1<C, C.T.T, S2<C.T>>' : 'P2'}}
    where C : P1,
          A == S1<C, C.T.T, S2<C.T>>,
          C.T == D,
          E == D.T { }
}

// Same as above but without unnecessary generic parameters.
struct WithoutBogusGenericParametersWithWarning<A, B> where A : P2, B : P2, A.T == B.T {
  // CHECK-LABEL: Generic signature: <A, B, C where A == S1<C, B.[P2]T.[P1]T, S2<B.[P2]T>>, B : P2, C : P1, B.[P2]T == C.[P1]T>
  init<C>(_: C) // expected-warning {{redundant conformance constraint 'S1<C, C.T.T, S2<C.T>>' : 'P2'}}
    where C : P1,
          C.T : P1, // expected-warning {{redundant conformance constraint 'C.T' : 'P1'}}
          A == S1<C, C.T.T, S2<C.T>> {}
}

// Same as above but without unnecessary generic parameters
// or the warning.
struct WithoutBogusGenericParametersWithoutWarning<A, B> where A : P2, B : P2, A.T == B.T {
  // CHECK-LABEL: Generic signature: <A, B, C where A == S1<C, B.[P2]T.[P1]T, S2<B.[P2]T>>, B : P2, C : P1, B.[P2]T == C.[P1]T>
  init<C>(_: C) // expected-warning {{redundant conformance constraint 'S1<C, C.T.T, S2<C.T>>' : 'P2'}}
    where C : P1,
          A == S1<C, C.T.T, S2<C.T>> {}
}
