// RUN: %target-swift-frontend -typecheck -enable-opt-in-reflection-metadata %s -verify

// MARK: - Correct usage of @reflectable attribute

@reflectable
protocol P {}

@reflectable
struct S {
  @reflectable
  struct S2 {}
  
  let s2: S2
}

@reflectable
enum E {}

@reflectable
class C {}

// MARK: - Errors

@reflectable // expected-error {{'@reflectable' attribute cannot be applied to this declaration}}
extension String {}

struct S2 {
  @reflectable // expected-error {{'@reflectable' attribute cannot be applied to this declaration}}
  func F() {}
}
