protocol P1 {}

struct S1 : P1 {}

func test(_ o: P1?) -> String {
  switch o {
  case let s as S1:
    test(s)
  default:
    test(o)
  }

  _ = "\(o)"
  return "\(o)"
}

// RUN: %sourcekitd-test -req=cursor -pos=7:17 %s -- %s | %FileCheck -check-prefix=CHECK1 %s
// CHECK1: source.lang.swift.ref.struct (3:8-3:10)
// CHECK1: S1

// RUN: %sourcekitd-test -req=cursor -pos=13:10 %s -- %s | %FileCheck -check-prefix=CHECK2 %s
// CHECK2: source.lang.swift.ref.var.local (5:13-5:14)
// CHECK2: (any P1)?

// RUN: %sourcekitd-test -req=cursor -pos=14:13 %s -- %s | %FileCheck -check-prefix=CHECK3 %s
// CHECK3: source.lang.swift.ref.var.local (5:13-5:14)
// CHECK3: (any P1)?
