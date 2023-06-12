// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-frontend -typecheck -verify -verify-ignore-unknown -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop

//--- Inputs/module.modulemap
module namespaces {
  header "test.h"
  requires cplusplus
}
//--- Inputs/test.h
namespace Parent {
inline namespace InlineChild {

void functionInInlineChild();

} // namespace InlineChild
} // namespace Parent

//--- test.swift

import namespaces;

// Swift's typechecker currently doesn't allow calling a function from inline namespace when it's referenced through the parent namespace.
func test() {
  Parent.functionInInlineChild() // expected-error {{type of expression is ambiguous without a type annotation}}
}
