// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

struct S: ~Copyable {
  let s: String
  init(_ s: String) { self.s = s }
  deinit { print("deiniting \(s)") }
}

func use(_ s: borrowing S) {
  print("using: \(s.s)")
}

@_silgen_name("f")
func f(_ c: consuming S) {
  repeat {
    use(c)
    c = S("2")
  } while false
}

func doit() {
  let s = S("1")
  f(s)
}

//      CHECK: using: 1
// CHECK-NEXT: deiniting 1
// CHECK-NEXT: deiniting 2
doit()
