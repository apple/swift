// RUN: %swift -typecheck %s -verify -target wasm32-unknown-wasip1-threads -parse-stdlib -DTHREADS
// RUN: %swift-ide-test -test-input-complete -source-filename=%s -target wasm32-unknown-wasip1-threads

#if targetEnvironment(threads)
  func underTargetEnvironmentThreads() {
    foo() // expected-error {{cannot find 'foo' in scope}}
  }
#endif
