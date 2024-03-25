// RUN: %batch-code-completion

func foo(_ x: Int) {}

struct Bar {
  func bar(withString: String) -> String {}
  func bar(withInt: Int) -> Int {}
}

func test() {
  foo(Bar().bar(#^COMPLETE^#))
}
// Ensure that we don't report the call pattern for `bar` as `Convertible`
// COMPLETE-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#withString: String#}[')'][#String#]; name=withString:
// COMPLETE-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#withInt: Int#}[')'][#Int#]; name=withInt: