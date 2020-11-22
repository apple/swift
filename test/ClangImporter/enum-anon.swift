// RUN: %target-swift-frontend -typecheck %s -enable-objc-interop -import-objc-header %S/Inputs/enum-anon.h -DDIAGS -verify

// REQUIRES: objc_interop

func testDiags() {
  let us2 = USConstant2
  let _: String = us2 // expected-error {{cannot convert value of type 'UInt16' to specified type 'String'}}
  let usVar2 = USVarConstant2
  let _: String = usVar2 // expected-error {{cannot convert value of type 'UInt16' to specified type 'String'}}
}
