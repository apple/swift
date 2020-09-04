// RUN: %target-typecheck-verify-swift

protocol P {}

protocol Q {}

protocol PP: P {}

var qp: Q.Protocol
var pp: P.Protocol = qp // expected-error{{cannot convert value of type 'Q.Protocol' to specified type 'P.Protocol'}}

var qt: Q.Type
qt = qp // expected-error{{cannot assign value of type 'Q.Protocol' to type 'Q.Type'}}
qp = qt // expected-error{{cannot assign value of type 'Q.Type' to type 'Q.Protocol'}}
var pt: P.Type = qt // expected-error{{cannot convert value of type 'Q.Type' to specified type 'P.Type'}}
pt = pp // expected-error{{cannot assign value of type 'P.Protocol' to type 'P.Type'}}
pp = pt // expected-error{{cannot assign value of type 'P.Type' to type 'P.Protocol'}}

var pqt: (P & Q).Type
pt = pqt
qt = pqt


var pqp: (P & Q).Protocol
pp = pqp // expected-error{{cannot assign value of type '(P & Q).Protocol' to type 'P.Protocol'}}
qp = pqp // expected-error{{cannot assign value of type '(P & Q).Protocol' to type 'Q.Protocol'}}

var ppp: PP.Protocol
pp = ppp // expected-error{{cannot assign value of type 'PP.Protocol' to type 'P.Protocol'}}

var ppt: PP.Type
pt = ppt

var at: Any.Type
at = pt

var ap: Any.Protocol
ap = pp // expected-error{{cannot assign value of type 'P.Protocol' to type 'Any.Protocol'}}
ap = pt // expected-error{{cannot assign value of type 'P.Type' to type 'Any.Protocol'}}

// Meta-metatypes

protocol Toaster {}
class WashingMachine : Toaster {}
class Dryer : WashingMachine {}
class HairDryer {}

let a: Toaster.Type.Protocol = (Toaster.Type).self
let b: Any.Type.Type = (Toaster.Type).self // expected-error {{cannot convert value of type '(Toaster.Type).Protocol' to specified type 'Any.Type.Type'}}
let c: Any.Type.Protocol = (Toaster.Type).self // expected-error {{cannot convert value of type '(Toaster.Type).Protocol' to specified type 'Any.Type.Protocol'}}
let d: Toaster.Type.Type = (WashingMachine.Type).self
let e: Any.Type.Type = (WashingMachine.Type).self
let f: Toaster.Type.Type = (Dryer.Type).self
let g: Toaster.Type.Type = (HairDryer.Type).self // expected-error {{cannot convert value of type '(HairDryer.Type).Type' to specified type 'Toaster.Type.Type'}}
let h: WashingMachine.Type.Type = (Dryer.Type).self // expected-error {{cannot convert value of type '(Dryer.Type).Type' to specified type 'WashingMachine.Type.Type'}}

func generic<T : WashingMachine>(_ t: T.Type) {
  let _: Toaster.Type.Type = type(of: t)
}

// rdar://problem/20780797
protocol P2 {
  init(x: Int)
  var elements: [P2] {get}
}

extension P2 {
  init() { self.init(x: 5) }
}

func testP2(_ pt: P2.Type) {
  pt.init().elements // expected-warning {{expression of type '[P2]' is unused}}
}

// rdar://problem/21597711
protocol P3 {
  func withP3(_ fn: (P3) -> ())
}

class Something {
  func takeP3(_ p: P3) { }
}


func testP3(_ p: P3, something: Something) {
  p.withP3(Something.takeP3(something))
}

func testIUOToAny(_ t: AnyObject.Type!) {
  let _: Any = t
}
