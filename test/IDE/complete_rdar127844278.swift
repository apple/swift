// RUN: %batch-code-completion

// rdar://127844278 - Make sure we don't attempt to favor one buildExpression
// call over the other when there's a child code completion.

protocol P {}
protocol Q: P {}

struct S<T>: P {
  private init() {}
}

extension S where T == () {
  init(a: Void) {}
}
extension S: Q where T == String {
  init(b: String) {}
}

@resultBuilder struct Builder {
  static func buildExpression<T: P>(_ x: T) -> T { x }
  static func buildExpression<T: Q>(_ x: T) -> T { x }

  static func buildBlock<T: P>(_ x: T) -> some P { x }
  static func buildBlock<T: Q>(_ x: T) -> some P { x }
}

func foo<T>(@Builder _: () -> T) where T: P {}
func foo<T>(@Builder _: () -> T) where T: Q {}

foo {
  S(#^COMPLETE^#)
}
// COMPLETE-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Void#}[')'][#S<()>#]; name=a:
// COMPLETE-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#b: String#}[')'][#S<String>#]; name=b:
