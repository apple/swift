// RUN: %target-typecheck-verify-swift

struct A {}
struct B {}

func other() -> A {}

func takesB(_: B) {}

func multipleLocalResults1() {
  func other() -> B {}
  let result = other()
  takesB(result)
}

func multipleLocalResults2() {
  func other() -> B {}
  let result = other()
  takesB(result)
  let other: Int = 123
  print(other)
}
