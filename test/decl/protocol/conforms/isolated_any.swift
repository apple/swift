// RUN: %target-swift-frontend -typecheck -enable-experimental-feature IsolatedAny -verify %s

struct A<T> {
  // expected-note @+1 {{candidate has non-matching type}}
  func enqueue(operation: @escaping @Sendable () async -> T) {}
}

protocol AnnotatedEnqueuer {
  associatedtype Result

  // expected-note @+1 {{protocol requires function}}
  func enqueue(operation: @escaping @isolated(any) () async -> Result)
}

// expected-error @+1 {{type 'A<T>' does not conform to protocol 'AnnotatedEnqueuer'}}
extension A : AnnotatedEnqueuer {}
