// RUN: %target-typecheck-verify-swift -swift-version 5 -experimental-multi-statement-closures

func isInt<T>(_ value: T) -> Bool {
  return value is Int
}

func maybeGetValue<T>(_ value: T) -> T? {
  return value
}

func random(_: Int) -> Bool { return false }

func mapWithMoreStatements(ints: [Int]) {
  let _ = ints.map { i in
    guard var actualValue = maybeGetValue(i) else {
      return String(0)
    }

    let value = actualValue + 1
    do {
      if isInt(i) {
        print(value)
      } else if value == 17 {
        print("seventeen!")
      }
    }

    while actualValue < 100 {
      actualValue += 1
    }

  my_repeat:
    repeat {
      print("still here")

      if i % 7 == 0 {
        break my_repeat
      }
    } while random(i)

    for j in 0..<i where j % 2 == 0 {
      if j % 7 == 0 {
        continue
      }
      print("even")
    }

    return String(value)
  }
}
