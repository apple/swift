// RUN: %target-typecheck-verify-swift

// Array types.
class Base1 {
  func f0(_ x: [Int]) { }
  func f0a(_ x: [Int]?) { }
  func f1(_ x: [[Int]]) { }
  func f1a(_ x: [[Int]]?) { }
  func f2(_ x: [([Int]) -> [Int]]) { }
  func f2a(_ x: [([Int]?) -> [Int]?]?) { }
}

class Derived1 : Base1 {
  override func f0(_ x: Array<Int>) { }
  override func f0a(_ x: Optional<Array<Int>>) { }
  override func f1(_ x: Array<Array<Int>>) { }
  override func f1a(_ x: Optional<Array<Array<Int>>>) { }
  override func f2(_ x: Array<(Array<Int>) -> Array<Int>>) { }
  override func f2a(_ x: Optional<Array<(Optional<Array<Int>>) -> Optional<Array<Int>>>>) { }
}


// Array types in generic specializations.
struct X<T> { }

func testGenericSpec() {
  _ = X<[Int]>()
}

// Array types for construction.
func constructArray(_ n: Int) {
  var ones = [Int](repeating: 1, count: n)
  ones[5] = 0

  var matrix = [[Float]]()
  matrix[1][2] = 3.14159

  var _: [Int?] = [Int?]()
}

// Fix-Its from the old syntax to the new.

typealias FixIt0 = Int[] // expected-error{{array types are now written with the brackets around the element type}}{{20-20=[}}{{23-24=}}

// Make sure preCheckExpression() properly folds member types.

class Outer {
  class Middle {
    class Inner {}
    class GenericInner<V> {}

    typealias Alias = Inner
  }

  class GenericMiddle<U> {
    class Inner {}
  }

  typealias Alias = Middle
}

class GenericOuter<T> {
  class Middle {
    class Inner {}
  }
}

func takesMiddle(_: [Outer.Middle]) {}

takesMiddle([Outer.Middle]())

func takesInner(_: [Outer.Middle.Inner]) {}

takesInner([Outer.Middle.Inner]())
takesInner([Outer.Alias.Inner]())
takesInner([Outer.Middle.Alias]())
takesInner([Outer.Alias.Alias]())

takesInner([array.Outer.Middle.Inner]())
takesInner([array.Outer.Alias.Inner]())
takesInner([array.Outer.Middle.Alias]())
takesInner([array.Outer.Alias.Alias]())

func takesMiddle(_: [GenericOuter<Int>.Middle]) {}

takesMiddle([GenericOuter<Int>.Middle]())

func takesInner(_: [GenericOuter<Int>.Middle.Inner]) {}

takesInner([GenericOuter<Int>.Middle.Inner]())
takesInner([array.GenericOuter<Int>.Middle.Inner]())

func takesMiddle(_: [Outer.GenericMiddle<Int>]) {}

takesMiddle([Outer.GenericMiddle<Int>]())

func takesInner(_: [Outer.GenericMiddle<Int>.Inner]) {}

takesInner([Outer.GenericMiddle<Int>.Inner]())
takesInner([array.Outer.GenericMiddle<Int>.Inner]())

func takesInner(_: [Outer.Middle.GenericInner<Int>]) {}

takesInner([Outer.Middle.GenericInner<Int>]())
takesInner([array.Outer.Middle.GenericInner<Int>]())

protocol HasAssocType {
  associatedtype A
}

func takesAssocType<T : HasAssocType>(_: T, _: [T.A], _: [T.A?]) {}

func passAssocType<T : HasAssocType>(_ t: T) {
  takesAssocType(t, [T.A](), [T.A?]())
}

// SR-11134

let sr_11134_1 = [[1, 2, 3][0]] // ok
let sr_11134_2 = [[1, 2, 3] [1]] // expected-warning {{unexpected subscript in array literal; did you mean to write two separate elements instead?}}
// expected-note@-1 {{add a separator between the elements}}{{28-28=,}}
// expected-note@-2 {{remove the space between the elements to silence this warning}}{{28-29=}}
let sr_11134_3 = [
  [1, 2, 3] [1] // expected-warning {{unexpected subscript in array literal; did you mean to write two separate elements instead?}}
// expected-note@-1 {{add a separator between the elements}}{{12-12=,}}
// expected-note@-2 {{remove the space between the elements to silence this warning}}{{12-13=}}
]

// Disallow trailing commas when types are parsed in expression contexts.
let _ = [Int,]() // expected-error {{expected ']' in array type}}
                 // expected-note@-1 {{to match this opening '['}}
let _ = [1,] // ok
