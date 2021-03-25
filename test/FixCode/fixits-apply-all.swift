// This tests whether we accept fixits from warnings without filtering.
// The particular diagnostics used are not important.

// RUN: %swift -typecheck -target %target-triple %s -fixit-all -emit-fixits-path %t.remap
// RUN: c-arcmt-test %t.remap | arcmt-test -verify-transformed-files %s.result

func foo() -> Int {
  do {
  } catch var err {
    goo(err)
  }
}
func goo(_ e: Error) {

}

@warn_unused_result(message="test message")
func warn_unused_result_removal() -> Int { return 5 }

@discardableResult func discardableResultOnVoidFunc() {}

@discardableResult func discardableResultOnNeverFunc() -> Never { fatalError() }
