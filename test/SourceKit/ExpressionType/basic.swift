func foo() -> Int  { return 1 }

func bar(f: Float) -> Float { return bar(f: 1) }

protocol P {}

func fooP(_ p: P) { fooP(p) }

class C {}

func ArrayC(_ a: [C]) {
	_ = a.count
	_ = a.description.count.advanced(by: 1).description
}

struct S {
  let val = 4
}
func DictS(_ a: [Int: S]) {
  _ = a[2]?.val.advanced(by: 1).byteSwapped
}

// RUN: %sourcekitd-test -req=collect-type %s -- %s | %FileCheck %s
// CHECK: (183, 202): Int
// CHECK: (183, 196): String
// CHECK: (183, 184): [C]
// CHECK: (203, 211): (Int) -> (Int) -> Int
// CHECK: (216, 217): Int
// CHECK: (257, 258): Int
// CHECK: (291, 332): ()
// CHECK: (291, 292): Int?
// CHECK: (295, 332): Int?
// CHECK: (295, 320): Int
