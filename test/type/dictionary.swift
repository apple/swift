// RUN: %target-typecheck-verify-swift

// Dictionary types.
class Base {
  func f0(_ d: [String: Int]) { }
  func f1(_ d: [String: [Int: Int]]) { }
}

class Derived : Base {
  override func f0(_ d: Dictionary<String, Int>) { }
  override func f1(_ d: Dictionary<String, Dictionary<Int, Int>>) { }
}

// Dictionary types in generic specializations.
struct X<T> { }

func testGenericSpec() {
  _ = X<[Int : Int]>()
}

// Dictionary types for construction.
func constructDictionary(_ n: Int) {
  var dict = [Int : String](minimumCapacity: n)
  dict[5] = "hello"
}

// Parse errors
var y1: [String : Int = ["hello" : 1] // expected-error{{expected ']' in dictionary type}}
  // expected-note @-1{{to match this opening '['}}
var y2: [String : ] // expected-error{{expected dictionary value type}}


struct NotHashable { }

var nh1 : [NotHashable : Int] // expected-error{{'NotHashable' does not conform to protocol 'Hashable'}}

struct Y<T> : Hashable {
  func hash(into hasher: inout Hasher) {}

  static func ==(this: Y<T>, other: Y<T>) -> Bool { return true }
}

let _ = [Y<Int>: Int]()
let _ = [Y<Int> : Int]()
let _ = [Y<Int> :Int]()

// Disallow trailing commas when types are parsed in expression contexts.
let _ = [Int: String,]() // expected-error {{expected ']' in dictionary type}}
                         // expected-note@-1 {{to match this opening '['}}
let _ = [1: "foo",] // ok
