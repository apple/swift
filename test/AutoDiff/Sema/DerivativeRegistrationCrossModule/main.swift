// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -primary-file %S/Inputs/a.swift -emit-module-path %t/a.swiftmodule
// SWIFT_ENABLE_TENSORFLOW
// Use `-enable-experimental-cross-file-derivative-registration` flag. To be removed soon.
// RUN: %target-swift-frontend -enable-experimental-cross-file-derivative-registration -emit-module -primary-file %S/Inputs/b.swift -emit-module-path %t/b.swiftmodule -I %t
// SWIFT_ENABLE_TENSORFLOW END
// "-verify-ignore-unknown" is for "<unknown>:0: note: 'init()' declared here"
// RUN: %target-swift-frontend-typecheck -verify -verify-ignore-unknown -I %t %s

// SR-12526: Fix cross-module deserialization crash involving `@derivative` attribute.

import a
import b

func foo(_ s: Struct) {
  // Without this error, SR-12526 does not trigger.
  // expected-error @+1 {{'Struct' initializer is inaccessible due to 'internal' protection level}}
  _ = Struct()
  _ = s.method(1)
}
