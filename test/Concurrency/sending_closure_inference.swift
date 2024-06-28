// RUN: %target-swift-frontend -swift-version 6 %s -emit-silgen | %FileCheck %s

// READ THIS! This file only contains tests that validate that the relevant
// function subtyping rules for sending work. Please do not put other tests in
// the file!

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}

/////////////////
// MARK: Tests //
/////////////////

// CHECK: sil private [ossa] @$s25sending_closure_inference38testAnonymousParameterSendingInferenceyyFySSYucfU_ : $@convention(thin) (@sil_sending @guaranteed String) -> () {
func testAnonymousParameterSendingInference() {
  let _: (sending String) -> () = {
    print($0)
  }
}

// CHECK: sil private [ossa] @$s25sending_closure_inference38testNamedOnlyParameterSendingInferenceyyFySSYucfU_ : $@convention(thin) (@sil_sending @guaranteed String) -> () {
func testNamedOnlyParameterSendingInference() {
  let _: (sending String) -> () = { x in
    print(x)
  }
}

// CHECK: sil private [ossa] @$s25sending_closure_inference38testNamedTypeParameterSendingInferenceyyFySSnYucfU_ : $@convention(thin) (@sil_sending @owned String) -> () {
func testNamedTypeParameterSendingInference() {
  let _: (sending String) -> () = { (x: sending String) in
    print(x)
  }
}

// CHECK: sil private [ossa] @$s25sending_closure_inference26testSendingResultInferenceyyFSSyYTcfU_ : $@convention(thin) () -> @sil_sending @owned String {
func testSendingResultInference() {
  let _: () -> sending String = { "" }
}

func testSendingResultOnClosure() {
  let _ = { (x: String) -> sending String in x }
}
