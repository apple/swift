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

    // CHECK: (pattern_named type='((any P) -> Void, ((any P)?) -> Void, ([any P]) -> Void, ([Array<any P>?]) -> Void) -> [String : () -> any P]' 'x1')
    // CHECK: (pattern_named type='((any P) -> Void, ((any P)?) -> Void, ([any P]) -> Void, ([Array<any P>?]) -> Void) -> [String : () -> any P]' 'x2')
    // CHECK: (pattern_named type='((any P & Q) -> Void, ((any P & Q)?) -> Void, ([any P & Q]) -> Void, ([Array<any P & Q>?]) -> Void) -> [String : () -> any P & Q]' 'x3')
    // CHECK: (pattern_named type='((any P & Q) -> Void, ((any P & Q)?) -> Void, ([any P & Q]) -> Void, ([Array<any P & Q>?]) -> Void) -> [String : () -> any P & Q]' 'x4')
    // CHECK: (pattern_named type='((any C & P) -> Void, ((any C & P)?) -> Void, ([any C & P]) -> Void, ([Array<any C & P>?]) -> Void) -> [String : () -> any C & P]' 'x5')
    // CHECK: (pattern_named type='((any C & P) -> Void, ((any C & P)?) -> Void, ([any C & P]) -> Void, ([Array<any C & P>?]) -> Void) -> [String : () -> any C & P]' 'x6')
  }
}
