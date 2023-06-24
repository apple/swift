// Check we don't crash when verifying debug info.
// Ideally this should print the output after loadable by address runs
// but there's no way of doing this in SIL (for IRGen passes).
// RUN: %target-swift-frontend -emit-sil %s -Onone \
// RUN:   -sil-verify-all -Xllvm -verify-di-holes -emit-ir \
// RUN:   -Xllvm -sil-print-debuginfo -g -o - | %FileCheck %s

struct m {
  let major: Int
  let minor: Int
  let n: Int
  let o: [String]
  let p: [String]
  init(major: Int, minor: Int, n: Int, o: [String], p: [String]) {
    self.major = major
    self.minor = minor
    self.n = n
    self.o = o
    self.p = p
  }
}

enum a {
  case any
  case b(m)
}

struct c<e>  {
  enum f {
    case g(a)
  }
}

struct h<i>{
  typealias j = i
  typealias d = j
  typealias f = c<d>.f
  subscript(identifier: d) -> f {
      return .g(.any)
  }
  func k(l: f, identifier: d) -> h {
    switch (l, self[identifier]) {
    default:
        return self
    }
  }
}

// CHECK: define internal ptr @"$s4main1mVwCP"
