// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/existentials -emit-module
// RUN: sed -ne '/\/\/ *DEMANGLE: /s/\/\/ *DEMANGLE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test %t/existentials -type-from-mangled=%t/input | %FileCheck %s --match-full-lines

func blackHole(_: Any...) {}

protocol P {}
protocol Q {}
class C {}

class D : C, P, Q {}

do {
  let e0: Any = D()
  let e1: AnyObject = D()

  let e2: P = D()
  let e4: P & C = D()
  let e3: P & AnyObject = D()

  let e5: P & Q = D()
  let e6: P & Q & C = D()
  let e7: P & Q & AnyObject = D()

  blackHole(e0, e1, e2, e3, e4, e5, e6, e7)
}

do {
  let e0: Any.Type = D.self
  let e1: AnyObject.Type = D.self

  let e2: P.Type = D.self
  let e4: (P & C).Type = D.self
  let e3: (P & AnyObject).Type = D.self

  let e5: (P & Q).Type = D.self
  let e6: (P & Q & C).Type = D.self
  let e7: (P & Q & AnyObject).Type = D.self

  blackHole(e0, e1, e2, e3, e4, e5, e6, e7)
}

do {
  let e0: Any.Protocol = Any.self
  let e1: AnyObject.Protocol = AnyObject.self

  let e2: P.Protocol = P.self
  let e4: (P & C).Protocol = (P & C).self
  let e3: (P & AnyObject).Protocol = (P & AnyObject).self

  let e5: (P & Q).Protocol = (P & Q).self
  let e6: (P & Q & C).Protocol = (P & Q & C).self
  let e7: (P & Q & AnyObject).Protocol = (P & Q & AnyObject).self

  blackHole(e0, e1, e2, e3, e4, e5, e6, e7)
}

// DEMANGLE: $sypD
// DEMANGLE: $syXlD
// DEMANGLE: $s12existentials1P_pD
// DEMANGLE: $s12existentials1P_AA1CCXcD
// DEMANGLE: $s12existentials1P_XlD
// DEMANGLE: $s12existentials1P_AA1QpD
// DEMANGLE: $s12existentials1P_AA1QAA1CCXcD
// DEMANGLE: $s12existentials1P_AA1QXlD

// FIXME(https://github.com/apple/swift/issues/65879): All of these should be existentials, i.e., prefixed with 'any'
// CHECK: Any
// CHECK: AnyObject
// CHECK: P
// CHECK: C & P
// CHECK: P & AnyObject
// CHECK: P & Q
// CHECK: C & P & Q
// CHECK: P & Q & AnyObject

// DEMANGLE: $sypXpD
// DEMANGLE: $syXlXpD
// DEMANGLE: $s12existentials1P_pXpD
// DEMANGLE: $s12existentials1P_XlXpD
// DEMANGLE: $s12existentials1P_AA1CCXcXpD
// DEMANGLE: $s12existentials1P_AA1QpXpD
// DEMANGLE: $s12existentials1P_AA1QAA1CCXcXpD
// DEMANGLE: $s12existentials1P_AA1QXlXpD

// CHECK: any Any.Type
// CHECK: any AnyObject.Type
// CHECK: any P.Type
// CHECK: any (P & AnyObject).Type
// CHECK: any (C & P).Type
// CHECK: any (P & Q).Type
// CHECK: any (C & P & Q).Type
// CHECK: any (P & Q & AnyObject).Type

// DEMANGLE: $sypmD
// DEMANGLE: $syXlmD
// DEMANGLE: $s12existentials1P_pmD
// DEMANGLE: $s12existentials1P_AA1CCXcmD
// DEMANGLE: $s12existentials1P_XlmD
// DEMANGLE: $s12existentials1P_AA1QpmD
// DEMANGLE: $s12existentials1P_AA1QAA1CCXcmD
// DEMANGLE: $s12existentials1P_AA1QXlmD

// FIXME(https://github.com/apple/swift/issues/65880): All of these should be of the form (any <existential>).Type
// CHECK: Any.Type
// CHECK: AnyObject.Type
// CHECK: P.Type
// CHECK: (C & P).Type
// CHECK: (P & AnyObject).Type
// CHECK: (P & Q).Type
// CHECK: (C & P & Q).Type
// CHECK: (P & Q & AnyObject).Type
