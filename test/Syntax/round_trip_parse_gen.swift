// RUN: %swift-syntax-test -input-source-filename %s -parse-gen > %t
// RUN: diff -u %s %t
// RUN: %swift-syntax-test -input-source-filename %s -parse-gen -print-node-kind > %t.withkinds
// RUN: diff -u %S/Outputs/round_trip_parse_gen.swift.withkinds %t.withkinds

class C {
  func bar(_ a: Int) {}
  func bar1(_ a: Float) -> Float { return -0.6 + 0.1 - 0.3 }
  func bar2(a: Int, b: Int, c:Int) -> Int { return 1 }
  func bar3(a: Int) -> Int { return 1 }
  func bar4(_ a: Int) -> Int { return 1 }
  func foo() {
    var a = /*comment*/"ab\(x)c"/*comment*/
    var b = /*comment*/+2/*comment*/
    bar(1)
    bar(+10)
    bar(-10)
    bar1(-1.1)
    bar1(1.1)
    var f = /*comments*/+0.1/*comments*/
    foo()
  }

  func foo1() {
    _ = bar2(a:1, b:2, c:2)
    _ = bar2(a:1 + 1, b:2 * 2 + 2, c:2 + 2)
    _ = bar2(a : bar2(a: 1, b: 2, c: 3), b: 2, c: 3)
    _ = bar3(a : bar3(a: bar3(a: 1)))
    _ = bar4(bar4(bar4(1)))
    _ = [1, 2, 3, 4]
    _ = [1:1, 2:2, 3:3, 4:4]
    _ = [bar3(a:1), bar3(a:1), bar3(a:1), bar3(a:1)]
    _ = ["a": bar3(a:1), "b": bar3(a:1), "c": bar3(a:1), "d": bar3(a:1)]
    foo(nil, nil, nil)
  }
  func boolAnd() -> Bool { return true && false }
  func boolOr() -> Bool { return true || false }

  func foo2() {
    _ = true ? 1 : 0
    _ = (true ? 1 : 0) ? (true ? 1 : 0) : (true ? 1 : 0)
    _ = (1, 2)
    _ = (first: 1, second: 2)
    _ = (1)
    _ = (first: 1)
    if !true {
      return
    }
  }

  func foo3() {
    _ = [Any]()
    _ = a.a.a
    _ = a.b
    _ = 1.a
    (1 + 1).a.b.foo
    _ = a as Bool || a as! Bool || a as? Bool
    _ = a is Bool
  }
}

typealias A = Any
typealias B = (Array<Array<Any>>.Element)
typealias C = [Int]
typealias D = [Int: String]
typealias E = Int?.Protocol
typealias F = [Int]!.Type

struct foo {
  struct foo {
    struct foo {
      func foo() {
      }
    }
  }
  struct foo {}
}

struct foo {
  @available(*, unavailable)
  struct foo {}
  public class foo {
    @available(*, unavailable)
    @objc(fooObjc)
    private static func foo() {}
  }
}

struct S<A, B, C, @objc D> where A:B, B==C, A : C, B.C == D.A, A.B: C.D {}

private struct S<A, B>: Base where A: B {
  private struct S: A, B {}
}

protocol P: class {}

func foo(_ _: Int,
         a b: Int = 3 + 2,
         _ c: Int = 2,
         d _: Int = true ? 2: 3,
         @objc e: X = true,
         f: inout Int,
         g: Int...) throws -> [Int: String] {}

func foo(_ a: Int) throws -> Int {}
func foo( a: Int) rethrows -> Int {}

struct C {
@objc
@available(*, unavailable)
private static override func foo<a, b, c>(a b: Int, c: Int) throws -> [Int] where a==p1, b:p2 { ddd }
func rootView() -> Label {}
static func ==() -> bool {}
static func !=<a, b, c>() -> bool {}
}

@objc
private protocol foo : bar where A==B {}
protocol foo { func foo() }
private protocol foo{}
@objc
public protocol foo where A:B {}

func tryfoo() {
  try foo()
  try! foo()
  try? foo()
  try! foo().bar().foo().bar()
}

func closure() {
  {[weak a,
    unowned(safe) self,
    b = 3,
    unowned(unsafe) c = foo().bar] in
  }
  {[] in }

  { [] a, b, _ -> Int in
    return 2
  }
  { [] (a: Int, b: Int, _: Int) -> Int in
    return 2
  }
  { [] a, b, _ throws -> Int in
    return 2
  }
  { [] (a: Int, _ b: Int) throws -> Int in
    return 2
  }
  { a, b in }
}
