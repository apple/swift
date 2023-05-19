// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

protocol Publisher<Output, Failure> {
  associatedtype Output
  associatedtype Failure
}

func foo<P: Publisher>(_ publisher: P) where P.Failure == Never

func test() {
  foo(#^COMPLETE^#)
  // Make sure we don’t crash
  // COMPLETE: Begin completions
}
