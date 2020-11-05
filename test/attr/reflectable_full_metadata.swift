// RUN: %target-swift-frontend -typecheck %s -verify

// MARK: - Errors

@reflectable // expected-warning {{the compiler emits reflection metadata in full, `@reflectable` is ignored}}
protocol P {}

@reflectable // expected-warning {{the compiler emits reflection metadata in full, `@reflectable` is ignored}}
enum E {}

@reflectable // expected-warning {{the compiler emits reflection metadata in full, `@reflectable` is ignored}}
class C {}
