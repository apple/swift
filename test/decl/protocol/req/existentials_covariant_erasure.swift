// RUN: %target-swift-frontend -typecheck -dump-ast %s | %FileCheck %s

// Test that covariant 'Self' references get erased to the existential base type
// when operating on existential values.

class C {}
protocol P {
  func lotsOfSelfFunc(
    _: (Self) -> Void,
    _: (Self?) -> Void,
    _: ([Self]) -> Void,
    _: ([Array<Self>?]) -> Void
  ) -> [String : () -> Self]

  var lotsOfSelfProp: (
    _: (Self) -> Void,
    _: (Self?) -> Void,
    _: ([Self]) -> Void,
    _: ([Array<Self>?]) -> Void
  ) -> [String : () -> Self] { get }
}
protocol Q {}

do {
  class C {}

  func testCovariantSelfErasure(p: P, pq: P & Q, pc: P & C) {
    let x1 = p.lotsOfSelfFunc
    let x2 = p.lotsOfSelfProp

    let x3 = pq.lotsOfSelfFunc
    let x4 = pq.lotsOfSelfProp

    let x5 = pc.lotsOfSelfFunc
    let x6 = pc.lotsOfSelfProp

    // CHECK: (pattern=pattern_named type='((P) -> Void, (P?) -> Void, ([P]) -> Void, ([Array<P>?]) -> Void) -> [String : () -> P]' "x1")
    // CHECK: (pattern=pattern_named type='((P) -> Void, (P?) -> Void, ([P]) -> Void, ([Array<P>?]) -> Void) -> [String : () -> P]' "x2")
    // CHECK: (pattern=pattern_named type='((P & Q) -> Void, ((P & Q)?) -> Void, ([P & Q]) -> Void, ([Array<P & Q>?]) -> Void) -> [String : () -> P & Q]' "x3")
    // CHECK: (pattern=pattern_named type='((P & Q) -> Void, ((P & Q)?) -> Void, ([P & Q]) -> Void, ([Array<P & Q>?]) -> Void) -> [String : () -> P & Q]' "x4")
    // CHECK: (pattern=pattern_named type='((C & P) -> Void, ((C & P)?) -> Void, ([C & P]) -> Void, ([Array<C & P>?]) -> Void) -> [String : () -> C & P]' "x5")
    // CHECK: (pattern=pattern_named type='((C & P) -> Void, ((C & P)?) -> Void, ([C & P]) -> Void, ([Array<C & P>?]) -> Void) -> [String : () -> C & P]' "x6")
  }
}
