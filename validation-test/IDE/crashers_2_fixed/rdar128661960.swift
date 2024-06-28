// RUN: %batch-code-completion

func takeClosure(closure: () -> Bool) {}

func test(someLocal: Int) {
  takeClosure {
    if case .begin(#^COMPLETE^#)
  }
}
// COMPLETE: someLocal
