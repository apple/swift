// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/extensions_A.swift
// RUN: %target-swift-frontend -emit-module -I %t -o %t %S/Inputs/extensions_B.swift
// RUN: %target-swift-frontend -emit-module -I %t -o %t %S/Inputs/extensions_C.swift
// RUN: %target-swift-frontend -typecheck %s -I %t -verify -enable-experimental-feature ExtensionImportVisibility

import extensions_A
import extensions_C
// expected-note 2{{add import of module 'extensions_B'}}{{1-1=import extensions_B\n}}
func test(x: X, y: Y<Z>) {
  x.XinB() // expected-error{{instance method 'XinB()' is not available due to missing import of defining module 'extensions_B'}}
  y.YinB() // expected-error{{instance method 'YinB()' is not available due to missing import of defining module 'extensions_B'}}

  x.XinC()
  y.YinC()
}
