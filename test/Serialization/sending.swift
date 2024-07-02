// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -strict-concurrency=complete -module-name sending_test -emit-module -o %t/sending_test.swiftmodule %S/Inputs/sending.swift

// RUN: %target-sil-opt -strict-concurrency=complete -module-name sending_test %t/sending_test.swiftmodule | %FileCheck -check-prefix=AST %s
// RUN: %target-swift-frontend -strict-concurrency=complete -module-name sending -emit-sil -I %t %s | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: asserts

// READ THIS!
//
// This test is meant to test serialization of sending declarations in the
// AST.

import sending_test

func main() {
  let x = "123"
  let _ = testSending(x)

  do {
    let f: (sending String) -> () = {
      (z: sending String) in
      print(z)
    }
    testSendingFunc(f)
  }

  do {
    let f: () -> sending String = {
      ""
    }
    testSendingResultFunc(f)
  }
}

// CHECK: sil @$s12sending_test0B7SendingyS2SnF : $@convention(thin) (@sil_sending @owned String) -> @sil_sending @owned String
// CHECK: sil @$s12sending_test0B11SendingFuncyyySSnYuXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed (@sil_sending @owned String) -> ()) -> ()
// CHECK: sil @$s12sending_test0B17SendingResultFuncyySSyYTXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @sil_sending @owned String) -> ()

// AST-LABEL: func testSending(_ x: sending String) -> sending String
// AST-LABEL: func testSendingFunc(_ x: (sending String) -> ())
// AST-LABEL: func testSendingResultFunc(_ x: () -> sending String)
