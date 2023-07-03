// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all -enforce-exclusivity=none -Xllvm -move-only-address-checker-disable-lifetime-extension=true) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all -enforce-exclusivity=none -Xllvm -move-only-address-checker-disable-lifetime-extension=true) | %FileCheck %s

// REQUIRES: executable_test

struct S: ~Copyable {
  let s: String
  init(_ s: String) { self.s = s }
  deinit { print("deiniting \(s)") }
}

struct M4 : ~Copyable {
  var s1: S
  var s2: S
  var s3: S
  var s4: S
  init(_ s: String) {
    self.s1 = S("\(s).s1")
    self.s2 = S("\(s).s2")
    self.s3 = S("\(s).s3")
    self.s4 = S("\(s).s4")
  }
}

func rewriteTwo(_ one: inout S, _ two: inout S) {
  print("entering \(#function)")
  one = S("new1")
  two = S("new2")
  print("exiting \(#function)")
}

func doit() {
  var m = M4("1")
  // CHECK: deiniting 1.s1
  // CHECK: deiniting 1.s2
  // CHECK: deiniting new1
  // CHECK: deiniting new2
  rewriteTwo(&m.s1, &m.s2)
}

doit()
