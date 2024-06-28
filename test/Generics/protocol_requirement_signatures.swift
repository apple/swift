// RUN: %target-typecheck-verify-swift
// RUN: %target-typecheck-verify-swift -debug-generic-signatures > %t.dump 2>&1
// RUN: %FileCheck %s < %t.dump

// CHECK-LABEL: .P1@
// CHECK-NEXT: Requirement signature: <Self>
protocol P1 {}

// CHECK-LABEL: .P2@
// CHECK-NEXT: Requirement signature: <Self>
protocol P2 {}

// CHECK-LABEL: .P3@
// CHECK-NEXT: Requirement signature: <Self>
protocol P3 {}

// basic protocol
// CHECK-LABEL: .Q1@
// CHECK-NEXT: Requirement signature: <Self where Self.[Q1]X : P1>
protocol Q1 {
    associatedtype X: P1 // expected-note {{declared here}}
}

// inheritance
// CHECK-LABEL: .Q2@
// CHECK-NEXT: Requirement signature: <Self where Self : Q1>
protocol Q2: Q1 {}

// inheritance without any new requirements
// CHECK-LABEL: .Q3@
// CHECK-NEXT: Requirement signature: <Self where Self : Q1>
protocol Q3: Q1 {
    associatedtype X
}

// inheritance adding a new conformance
// CHECK-LABEL: .Q4@
// CHECK-NEXT: Requirement signature: <Self where Self : Q1, Self.[Q1]X : P2>
protocol Q4: Q1 {
    associatedtype X: P2 // expected-warning{{redeclaration of associated type 'X'}} // expected-note 2{{declared here}}
}

// multiple inheritance
// CHECK-LABEL: .Q5@
// CHECK-NEXT: Requirement signature: <Self where Self : Q2, Self : Q3, Self : Q4>
protocol Q5: Q2, Q3, Q4 {}

// multiple inheritance without any new requirements
// CHECK-LABEL: .Q6@
// CHECK-NEXT: Requirement signature: <Self where Self : Q2, Self : Q3, Self : Q4>
protocol Q6: Q2,
             Q3, Q4 {
    associatedtype X: P1
                   // expected-warning@-1{{redeclaration of associated type 'X' from protocol 'Q4' is}}
}

// multiple inheritance with a new conformance
// CHECK-LABEL: .Q7@
// CHECK-NEXT: Requirement signature: <Self where Self : Q2, Self : Q3, Self : Q4, Self.[Q1]X : P3>
protocol Q7: Q2, Q3, Q4 {
    associatedtype X: P3 // expected-warning{{redeclaration of associated type 'X'}}
}

// https://github.com/apple/swift/issues/48504

class SomeBaseClass {}

// CHECK-DAG: .P4@
// CHECK-NEXT: Requirement signature: <Self where Self == Self.[P4]BType.[P5]AType, Self.[P4]BType : P5, Self.[P4]Native : SomeBaseClass>
protocol P4 {
	associatedtype Native : SomeBaseClass
  associatedtype BType : P5 where BType.AType == Self
}

// CHECK-DAG: .P5@
// CHECK-NEXT: <Self where Self == Self.[P5]AType.[P4]BType, Self.[P5]AType : P4>
protocol P5 {
	associatedtype AType : P4 where AType.BType == Self
}

// https://github.com/apple/swift/issues/50651

protocol P6 {
  associatedtype A1: P7
}

// CHECK-DAG: .P7@
// CHECK-NEXT: <Self where Self == Self.[P7]A2.[P6]A1, Self.[P7]A2 : P6>
protocol P7 {
  associatedtype A2: P6 where A2.A1 == Self
}
