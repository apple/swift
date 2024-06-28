// RUN: %target-swift-ide-test -code-completion -code-completion-token=COMPLETE -source-filename=%s

actor Foo {
  func bar() {}
}

func takeClosure(_ x: () -> Void) {}

actor Foo {
  func tests(myInt: Int) {
    takeClosure {
      myInt.#^COMPLETE^#
    }
  }
}
