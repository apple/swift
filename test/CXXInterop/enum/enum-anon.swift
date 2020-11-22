// RUN: %target-swift-frontend -typecheck %s -I  %S/Inputs -DDIAGS -verify
// RUN: %target-swift-frontend -typecheck %s -enable-cxx-interop -I %S/Inputs -DDIAGS -verify
import EnumAnon

func testDiags() {

  let us2: UInt16 = 0
  let _: String = us2 // expected-error {{cannot convert value of type 'UInt16' to specified type 'String'}}

  let usVar2: UInt16 = 0
  let _: String = usVar2 // expected-error {{cannot convert value of type 'UInt16' to specified type 'String'}}

  // The nested anonymous enum value should still have top-level scope, because
  // that's how C works. It should also have the same type as the field (above).
  let _: String = SR2511.SR2511B // expected-error {{type 'SR2511' has no member 'SR2511B'}}
}

