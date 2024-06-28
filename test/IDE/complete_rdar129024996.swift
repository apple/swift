// RUN: %batch-code-completion

protocol P {}
protocol Q : P {}

// Applicable, S conforms to Q.
struct S : Q {}
extension P where Self == S {
  static func foo() -> S { S() }
}

// Not applicable, Q does not inherit from K.
protocol K {}
extension S : K {}
extension K where Self == S {
  static func bar() -> S { S() }
}

// Make sure we don't try and complete for this type's init.
struct R {
  init(a: Int) {}
}

// Not applicable, A does not conform to Q.
struct A: P {}
extension P where Self == A {
  static func baz() -> A { A() }
}

struct B<T>: P {}
extension B: Q where T: Q {}

// Applicable, B<S> conforms to Q.
extension P where Self == B<S> {
  static func qux() -> Self { .init() }
}

// Not applicable, B<A> does not conform to Q.
extension P where Self == B<A> {
  static func flim() -> Self { .init() }
}

@attached(peer) macro R(_: any Q) = #externalMacro(module: "", type: "")

@R(.#^COMPLETE^#)
func bar() {}
// COMPLETE:     Begin completions, 2 items
// COMPLETE-DAG: Decl[StaticMethod]/Super/TypeRelation[Convertible]: foo()[#S#]; name=foo()
// COMPLETE-DAG: Decl[StaticMethod]/Super/TypeRelation[Convertible]: qux()[#B<S>#]; name=qux()
