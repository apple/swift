// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test
struct Alice: ~Copyable {
  var age: Int

  init(age: Int) {
    print("INIT");
    self.age = age
  }

  deinit { print("DEINIT") }
}

func eatMe(_ alice: consuming Alice) {
  print(" start")
  print(" age:", alice.age)
  print(" end")
}

func doit() {
  let alice = Alice(age: 10)
  eatMe(alice)
}

doit()

// CHECK: INIT
// CHECK:  start
// CHECK:  age: 10
// CHECK:  end
// CHECK: DEINIT
