// RUN: %target-swift-frontend -typecheck -dump-ast %s | %FileCheck %s

protocol P1 {
  associatedtype A
}

// CHECK-LABEL: (protocol{{.*}}"P2"
// CHECK-NEXT: (generic_sig
// CHECK-NEXT: (associated_type_decl{{.*}}"A" {{.*}} overridden=P1))
protocol P2 : P1 {
  associatedtype A
}

protocol P3 {
  associatedtype A
}

// CHECK-LABEL: (protocol{{.*}}"P4"
// CHECK-NEXT: (generic_sig
// CHECK-NEXT: (associated_type_decl{{.*}}"A" {{.*}} overridden=P2, P3))
protocol P4 : P2, P3 {
  associatedtype A
}

// CHECK-LABEL: (protocol{{.*}}"P5"
// CHECK-NEXT: (generic_sig
// CHECK-NEXT: (associated_type_decl{{.*}}"A" {{.*}} overridden=P4))
protocol P5 : P4, P2 {
  associatedtype A
}

