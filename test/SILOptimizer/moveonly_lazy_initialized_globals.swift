// RUN: %target-swift-emit-sil -sil-verify-all -parse-as-library -verify %s

struct S: ~Copyable {
  let s: String
  init(_ s: String) { self.s = s }
  deinit { print("deiniting \(s)") }
}

struct M4 : ~Copyable {
  var s1: S
  var s2: S
  init(_ s: String) {
      fatalError()
  }
}

func rewriteTwo(_ one: inout S, _ two: inout S) {
  one = S("new1")
  two = S("new2")
}

var m = M4("1")
var m2 = M4("1")

struct Doit {
  static var m3 = M4("1")
  static var m4 = M4("1")

  // We should get no diagnostics.
  static func run() {
      rewriteTwo(&m.s1, &m2.s2)
      rewriteTwo(&m3.s1, &m4.s2)
  }
}
