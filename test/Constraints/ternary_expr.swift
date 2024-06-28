// RUN: %target-typecheck-verify-swift

func useInt(_ x: Int) {}
func useDouble(_ x: Double) {}

class B {
  init() {} 
}
class D1 : B {
  override init() { super.init() } 
}
class D2 : B {
  override init() { super.init() } 
}

func useB(_ x: B) {}
func useD1(_ x: D1) {}
func useD2(_ x: D2) {}

var a = true ? 1 : 0 // should infer Int
var b : Double = true ? 1 : 0 // should infer Double
var c = true ? 1 : 0.0 // should infer Double
var d = true ? 1.0 : 0 // should infer Double

useInt(a)
useDouble(b)
useDouble(c)
useDouble(d)

var z = true ? a : b // expected-error{{result values in '? :' expression have mismatching types 'Int' and 'Double'}}
var _ = a ? b : b // expected-error{{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}



var e = true ? B() : B() // should infer B
var f = true ? B() : D1() // should infer B
var g = true ? D1() : B() // should infer B
var h = true ? D1() : D1() // should infer D1
var i = true ? D1() : D2() // should infer B

useB(e)
useD1(e) // expected-error{{cannot convert value of type 'B' to expected argument type 'D1'}}
useB(f)
useD1(f) // expected-error{{cannot convert value of type 'B' to expected argument type 'D1'}}
useB(g)
useD1(g) // expected-error{{cannot convert value of type 'B' to expected argument type 'D1'}}
useB(h)
useD1(h)
useB(i)
useD1(i) // expected-error{{cannot convert value of type 'B' to expected argument type 'D1'}}
useD2(i) // expected-error{{cannot convert value of type 'B' to expected argument type 'D2'}}

var x = true ? 1 : 0
var y = 22 ? 1 : 0 // expected-error{{cannot convert value of type 'Int' to expected condition type 'Bool'}}

_ = x ? x : x // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
_ = true ? x : 1.2 // expected-error {{result values in '? :' expression have mismatching types 'Int' and 'Double'}}

_ = (x: true) ? true : false // expected-error {{cannot convert value of type '(x: Bool)' to expected condition type 'Bool'}}
_ = (x: 1) ? true : false // expected-error {{cannot convert value of type '(x: Int)' to expected condition type 'Bool'}}

func resultBool() -> Bool { true }
_ = resultBool ? true : false // expected-error {{function 'resultBool' was used as a property; add () to call it}} {{15-15=()}}

let ib: Bool! = false
let eb: Bool? = .some(false)
let conditional = ib ? "Broken" : "Heart" // should infer Bool!
let conditional = eb ? "Broken" : "Heart" // expected-error {{value of optional type 'Bool?' must be unwrapped}}
// expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}
// expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}

// <rdar://problem/39586166> - crash when TernaryExpr has UnresolvedType in condition
struct Delegate {
  var shellTasks: [ShellTask]
}

extension Array {
  subscript(safe safe: Int) -> Element? {
    get { }
    set { }
  }
}

struct ShellTask {
  var commandLine: [String]
}

let delegate = Delegate(shellTasks: [])
_ = delegate.shellTasks[safe: 0]?.commandLine.compactMap({ $0.asString.hasPrefix("") ? $0 : nil }).count ?? 0
// expected-error@-1 {{value of type 'String' has no member 'asString'}}
