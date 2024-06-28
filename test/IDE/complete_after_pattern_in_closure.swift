// RUN: %batch-code-completion -enable-experimental-concurrency

func makeURL(withExtension ext: Int?) -> Int? {
  return nil
}

func test() {
  let calculatorContext: Int? = {
    guard let url#^COMPLETE^# = makeURL(withExtension: 1),
          let script = url else {
      return nil
    }
  }()
  // COMPLETE-NOT: Begin completions
}
