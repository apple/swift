// RUN: %target-typecheck-verify-swift -warn-redundant-requirements
// RUN: %target-swift-frontend -typecheck -debug-generic-signatures %s 2>&1 | %FileCheck %s

struct G<T> {}

// CHECK-LABEL: Generic signature: <T where T == any Error>
extension G where T : Error, T == Error {}
// expected-warning@-1 {{redundant conformance constraint 'any Error' : 'Error'}}
