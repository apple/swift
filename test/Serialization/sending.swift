// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -strict-concurrency=complete -enable-upcoming-feature RegionBasedIsolation -module-name transferring_test -emit-module -o %t/transferring_test.swiftmodule %S/Inputs/sending.swift
// RUN: %target-swift-frontend -strict-concurrency=complete -enable-upcoming-feature RegionBasedIsolation -module-name transferring -emit-sil -I %t %s | %FileCheck %s
// RUN: %target-sil-opt -strict-concurrency=complete -module-name transferring_test -enable-upcoming-feature RegionBasedIsolation %t/transferring_test.swiftmodule | %FileCheck -check-prefix=AST %s

// REQUIRES: concurrency
// REQUIRES: asserts

// READ THIS!
//
// This test is meant to test serialization of transferring declarations in the
// AST.

import transferring_test

func main() {
  let x = "123"
  let y = testTransferring(x)
  let _ = testSending(y)

  do {
    let f: (transferring String) -> () = {
      (z: transferring String) in
      print(z)
    }
    testTransferringFunc(f)
  }

  do {
    let f: (sending String) -> () = {
      (z: sending String) in
      print(z)
    }
    testSendingFunc(f)
  }

  do {
    let f: () -> transferring String = {
      ""
    }
    testTransferringResultFunc(f)
  }

  do {
    let f: () -> sending String = {
      ""
    }
    testSendingResultFunc(f)
  }
}

// CHECK: sil @$s17transferring_test0B12TransferringyS2SnF : $@convention(thin) (@sil_sending @owned String) -> @sil_sending @owned String
// CHECK: sil @$s17transferring_test0B7SendingyS2SnF : $@convention(thin) (@sil_sending @owned String) -> @sil_sending @owned String
// CHECK: sil @$s17transferring_test0B16TransferringFuncyyySSnYuXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed (@sil_sending @owned String) -> ()) -> ()
// CHECK: sil @$s17transferring_test0B11SendingFuncyyySSnYuXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed (@sil_sending @owned String) -> ()) -> ()
// CHECK: sil @$s17transferring_test0B22TransferringResultFuncyySSyYTXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @sil_sending @owned String) -> ()
// CHECK: sil @$s17transferring_test0B17SendingResultFuncyySSyYTXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @sil_sending @owned String) -> ()

// AST-LABEL: func testTransferring(_ x: sending String) -> sending String
// AST-LABEL: func testSending(_ x: sending String) -> sending String
// AST-LABEL: func testTransferringFunc(_ x: (sending String) -> ())
// AST-LABEL: func testSendingFunc(_ x: (sending String) -> ())
// AST-LABEL: func testTransferringResultFunc(_ x: () -> sending String)
// AST-LABEL: func testSendingResultFunc(_ x: () -> sending String)
