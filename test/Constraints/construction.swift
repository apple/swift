// RUN: %target-typecheck-verify-swift

struct X {
  var i : Int, j : Int
}

struct Y {
  init (_ x : Int, _ y : Float, _ z : String) {}
}

enum Z {
  case none
  case char(UnicodeScalar)
  case string(String)
  case point(Int, Int)

  init() { self = .none }
  init(_ c: UnicodeScalar) { self = .char(c) }
  // expected-note@-1 2 {{candidate expects value of type 'UnicodeScalar' (aka 'Unicode.Scalar') for parameter #1 (got 'Z')}}
  init(_ s: String) { self = .string(s) }
  // expected-note@-1 2 {{candidate expects value of type 'String' for parameter #1 (got 'Z')}}
  init(_ x: Int, _ y: Int) { self = .point(x, y) }
}

enum Optional<T> {
  case none
  case value(T)

  init() { self = .none }
  init(_ t: T) { self = .value(t) }
}

class Base { }
class Derived : Base { }

var d : Derived
typealias Point = (x : Int, y : Int)
var hello : String = "hello";
var world : String = "world";
var i : Int
var z : Z = .none

func acceptZ(_ z: Z) {}
func acceptString(_ s: String) {}

Point(1, 2) // expected-warning {{expression of type '(x: Int, y: Int)' is unused}}
var db : Base = d
X(i: 1, j: 2) // expected-warning{{unused}}
Y(1, 2, "hello") // expected-warning{{unused}}

// Unions
Z(UnicodeScalar("a")) // expected-warning{{unused}}
Z(1, 2) // expected-warning{{unused}}

acceptZ(.none)
acceptZ(.char("a"))
acceptString("\(hello), \(world) #\(i)!")

Optional<Int>(1) // expected-warning{{unused}}
Optional(1) // expected-warning{{unused}}
_ = .none as Optional<Int>
Optional(.none) // expected-error {{cannot infer contextual base in reference to member 'none'}}

// Interpolation
_ = "\(hello), \(world) #\(i)!"

class File {
  init() { 
    fd = 0
    body = ""
  }

  var fd : Int32, body : String
    func replPrint() {
    print("File{\n  fd=\(fd)\n  body=\"\(body)\"\n}", terminator: "")
  }
}

// Non-trivial references to metatypes.
struct Foo { 
  struct Inner { }
}
extension Foo {
  func getInner() -> Inner {
    return Inner()
  }
}

// Downcasting
var b : Base
_ = b as! Derived

// Construction doesn't permit conversion.
// NOTE: Int and other integer-literal convertible types
//  are special cased in the library.
Int(i) // expected-warning{{unused}}
_ = i as Int
Z(z) // expected-error{{no exact matches in call to initializer}}

Z.init(z)  // expected-error {{no exact matches in call to initializer}}


_ = z as Z

// Construction from inouts.
struct FooRef { }
struct BarRef {
  init(x: inout FooRef) {}
  init(x: inout Int) {}
}
var f = FooRef()
var x = 0
BarRef(x: &f) // expected-warning{{unused}}
BarRef(x: &x) // expected-warning{{unused}}

// Construction from a Type value not immediately resolved.
struct S1 {
  init(i: Int) { }
}

struct S2 {
  init(i: Int) { }
}

func getMetatype(_ i: Int) -> S1.Type { return S1.self }
func getMetatype(_ d: Double) -> S2.Type { return S2.self }

var s1 = getMetatype(1).init(i: 5)
s1 = S1(i: 5)

var s2 = getMetatype(3.14).init(i: 5)
s2 = S2(i: 5)

// rdar://problem/19254404
let i32 = Int32(123123123)
Int(i32 - 2 + 1) // expected-warning{{unused}}

// rdar://problem/19459079
let xx: UInt64 = 100
let yy = ((xx + 10) - 5) / 5
let zy = (xx + (10 - 5)) / 5

// rdar://problem/30588177
struct S3 {
  init() { }
}

let s3a = S3()

extension S3 {
  init?(maybe: S3) { return nil }
}

let s3b = S3(maybe: s3a)

// SR-5245 - Erroneous diagnostic - Type of expression is ambiguous without more context
class SR_5245 {
    struct S {
        enum E {
            case e1
            case e2
        }

        let e: [E]
    }

    init(s: S) {}
}

SR_5245(s: SR_5245.S(f: [.e1, .e2]))
// expected-error@-1 {{incorrect argument label in call (have 'f:', expected 'e:')}} {{22-23=e}}

// rdar://problem/34670592 - Compiler crash on heterogeneous collection literal
_ = Array([1, "hello"]) // Ok

func init_via_non_const_metatype(_ s1: S1.Type) {
  _ = s1(i: 42) // expected-error {{initializing from a metatype value must reference 'init' explicitly}} {{9-9=.init}}
  _ = s1.init(i: 42) // ok
}

// rdar://problem/45535925 - diagnostic is attached to invalid source location
func rdar_45535925() {
  struct S {
    var addr: String
    var port: Int

    private init(addr: String, port: Int?) {
      // expected-note@-1 {{'init(addr:port:)' declared here}}
      self.addr = addr
      self.port = port ?? 31337
    }

    private init(port: Int) {
      self.addr = "localhost"
      self.port = port
    }

    private func foo(_: Int) {}  // expected-note {{'foo' declared here}}
    private static func bar() {} // expected-note {{'bar()' declared here}}
  }

  _ = S(addr: "localhost", port: nil)
  // expected-error@-1 {{'S' initializer is inaccessible due to 'private' protection level}}

  func baz(_ s: S) {
    s.foo(42)
    // expected-error@-1 {{'foo' is inaccessible due to 'private' protection level}}
    S.bar()
    // expected-error@-1 {{'bar' is inaccessible due to 'private' protection level}}
  }
}

// rdar://problem/50668864
func rdar_50668864() {
  struct Foo {
    init(anchors: [Int]) { // expected-note {{'init(anchors:)' declared here}}
      self = .init { _ in [] } // expected-error {{trailing closure passed to parameter of type '[Int]' that does not accept a closure}}
    }
  }
}

// SR-10837 (rdar://problem/51442825) - init partial application regression
func sr_10837() {
  struct S {
    let value: Int

    static func foo(_ v: Int?) {
      _ = v.flatMap(self.init(value:)) // Ok
      _ = v.flatMap(S.init(value:))    // Ok
      _ = v.flatMap { S.init(value:)($0) }    // Ok
      _ = v.flatMap { self.init(value:)($0) } // Ok
    }
  }

  class A {
    init(bar: Int) {}
  }

  class B : A {
    init(value: Int) {}
    convenience init(foo: Int = 42) {
      self.init(value:)(foo) // Ok
      self.init(value:)
      // expected-error@-1 {{calling a 'self.init' initializer delegation without an argument list is not allowed}}
    }
  }

  class C : A {
    override init(bar: Int) {
      super.init(bar:)(bar) // Ok
      super.init(bar:)
      // expected-error@-1 {{calling a 'super.init' initializer chain without an argument list is not allowed}}
    }
  }
}

// To make sure that hack related to type variable bindings works as expected we need to test
// that in the following case result of a call to `reduce` maintains optionality.
func test_that_optionality_of_closure_result_is_preserved() {
  struct S {}

  let arr: [S?] = []
  let _: [S]? = arr.reduce([], { (a: [S]?, s: S?) -> [S]? in
    a.flatMap { (group: [S]) -> [S]? in s.map { group + [$0] } } // Ok
  })
}
