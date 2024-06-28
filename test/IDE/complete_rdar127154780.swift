// RUN: %batch-code-completion

// rdar://127154780 - Make sure we provide completions on variables that rely
// on result builders being solved.

@resultBuilder
enum Builder {
  static func buildBlock<T>(_ x: T) -> T { x }
}

struct S {}

struct R<T> {
  init(@Builder _: () -> T) {}
}

extension R where T == S {
  func bar() {}
}

func foo() {
  let r = R() {
    S()
  }
  r.#^COMPLETE1?check=COMPLETE^#

  let fn = {
    let r = R() {
      S()
    }
    r.#^COMPLETE2?check=COMPLETE^#
  }
}
// COMPLETE-DAG: Keyword[self]/CurrNominal:          self[#R<S>#]; name=self
// COMPLETE-DAG: Decl[InstanceMethod]/CurrNominal:   bar()[#Void#]; name=bar()
