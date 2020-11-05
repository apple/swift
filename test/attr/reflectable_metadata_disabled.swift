// RUN: %target-swift-frontend -disable-reflection-metadata -typecheck %s -verify

// MARK: - Errors

@reflectable // expected-warning {{`@reflectable` is ignored because reflection metadata is disabled}}
protocol P {}

@reflectable // expected-warning {{`@reflectable` is ignored because reflection metadata is disabled}}
enum E {}

@reflectable // expected-warning {{`@reflectable` is ignored because reflection metadata is disabled}}
class C {}
