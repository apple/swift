// RUN: %empty-directory(%t)
// RUN: %target-build-swift -g %s -o %t/bin
// RUN: %target-codesign %t/bin
// RUN: %target-run %t/bin | %FileCheck %s

// REQUIRES: executable_test

protocol Boopable: ~Copyable {
  func boop()
  mutating func bonk()
}

struct S: ~Copyable, Boopable {
  func boop() { print("boop") }
  mutating func bonk() { print("hmm") }
}

func borrow(_ b: borrowing any Boopable & ~Copyable) {
  b.boop()
}

func mutate(_ b: inout any Boopable & ~Copyable) {
  b.bonk()
}

// CHECK: boop
// CHECK: hmm
borrow(S())
var s = S() as any Boopable & ~Copyable
mutate(&s)
