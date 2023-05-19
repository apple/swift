// RUN: %target-typecheck-verify-swift -disable-availability-checking

//===----------------------------------------------------------------------===//
// Use of protocols with Self or associated type requirements
//===----------------------------------------------------------------------===//

struct Struct<T> {
  class Inner {}
  struct InnerGeneric<U> {}
}
class Class<T> {}

protocol P1 {
  associatedtype Q

  // Methods
  func covariantSelf1() -> Self
  func covariantSelf2() -> Self?
  func covariantSelf3() -> Self.Type
  func covariantSelf4() -> (Self, Self)
  func covariantSelf5() -> Array<Self>
  func covariantSelf6() -> [String : Self]
  func covariantSelf7(_: (Self) -> Void)
  func covariantSelf8(_: (Self...) -> Void)
  func covariantSelfComplex(_: (Self.Type) -> Void,
                            _: (Self.Type...) -> Void,
                            _: (Array<Self>) -> Void,
                            _: (Array<Array<Self>?>) -> Void,
                            _: (() -> Self?) -> Void
                           ) -> [String : () -> (Self, Self)]
  func covariantAssoc1() -> Q
  func covariantAssoc2() -> Q?
  func covariantAssoc3() -> Q.Type
  func covariantAssoc4() -> (Q, Q)
  func covariantAssoc5() -> Array<Q>
  func covariantAssoc6() -> [String : Q]
  func covariantAssoc7(_: (Q) -> Void)
  func covariantAssoc8(_: (Q...) -> Void)
  func covariantAssocComplex(_: (Q.Type) -> Void,
                             _: (Q.Type...) -> Void,
                             _: (Array<Q>) -> Void,
                             _: (Array<Array<Q>?>) -> Void,
                             _: (() -> Q?) -> Void
                            ) -> [String : () -> (Q, Q)]

  func contravariantSelf1(_: Self?)
  func contravariantSelf2(_: () -> Self)
  func contravariantSelf3(_: Array<() -> Self>)
  func contravariantSelf4(_: [String : () -> Self])
  func contravariantSelf5(_: () -> (Self, Self))
  func contravariantSelf6(_: ((Self) -> Void) -> Void)
  func contravariantSelf7() -> (Self) -> Void
  func contravariantSelf8() -> Array<((Self) -> Void)?>
  func contravariantSelf9(_: [String : (() -> Self)?])
  func contravariantSelf10() -> (Array<[String : Self??]>) -> Void
  func contravariantSelf11(_: Self.Type)
  func contravariantAssoc1(_: Q?)
  func contravariantAssoc2(_: () -> Q)
  func contravariantAssoc3(_: Array<() -> Q>)
  func contravariantAssoc4(_: [String : () -> Q])
  func contravariantAssoc5(_: () -> (Q, Q))
  func contravariantAssoc6(_: ((Q) -> Void) -> Void)
  func contravariantAssoc7() -> (Q) -> Void
  func contravariantAssoc8() -> Array<((Q) -> Void)?>
  func contravariantAssoc9(_: [String : (() -> Q)?])
  func contravariantAssoc10() -> (Array<[String : Q??]>) -> Void
  func contravariantAssoc11(_: Q.Type)

  func invariantSelf1(_: inout Self)
  func invariantSelf2(_: (inout Self) -> Void)
  func invariantSelf3(_: inout Array<() -> Self>)
  func invariantSelf4(_: Struct<Self>)
  func invariantSelf5() -> Struct<Self>
  func invariantSelf6() -> Struct<Self>.Inner
  func invariantSelf7(_: (Struct<Self>) -> Void)
  func invariantSelf8(_: Struct<(Self) -> Void>)
  func invariantSelf9(_: Struct<() -> Self>)
  func invariantSelf10(_: any P1 & Class<Self>)
  func invariantSelf11() -> Struct<Self>.InnerGeneric<Void>
  func invariantAssoc1(_: inout Q)
  func invariantAssoc2(_: (inout Q) -> Void)
  func invariantAssoc3(_: inout Array<() -> Q>)
  func invariantAssoc4(_: Struct<Q>)
  func invariantAssoc5() -> Struct<Q>
  func invariantAssoc6() -> Struct<Q>.Inner
  func invariantAssoc7(_: (Struct<Q>) -> Void)
  func invariantAssoc8(_: Struct<(Q) -> Void>)
  func invariantAssoc9(_: Struct<() -> Q>)
  func invariantAssoc10(_: any P1 & Class<Q>)
  func invariantAssoc11() -> Struct<Q>.InnerGeneric<Void>

  // Properties
  var covariantSelfProp1: Self { get }
  var covariantSelfProp2: Self? { get }
  var covariantSelfProp3: Self.Type { get }
  var covariantSelfProp4: (Self, Self) { get }
  var covariantSelfProp5: Array<Self> { get }
  var covariantSelfProp6: [String : Self] { get }
  var covariantSelfProp7: ((Self) -> Void) -> Void { get }
  var covariantSelfProp8: ((Self...) -> Void) -> Void { get }
  var covariantSelfPropComplex: ((Self.Type) -> Void,
                                 (Self.Type...) -> Void,
                                 (Array<Self>) -> Void,
                                 (Array<Array<Self>?>) -> Void,
                                 (() -> Self?) -> Void
                                ) -> [String : () -> (Self, Self)] { get }
  var covariantAssocProp1: Q { get }
  var covariantAssocProp2: Q? { get }
  var covariantAssocProp3: Q.Type { get }
  var covariantAssocProp4: (Q, Q) { get }
  var covariantAssocProp5: Array<Q> { get }
  var covariantAssocProp6: [String : Q] { get }
  var covariantAssocProp7: ((Q) -> Void) -> Void { get }
  var covariantAssocProp8: ((Q...) -> Void) -> Void { get }
  var covariantAssocPropComplex: ((Q.Type) -> Void,
                                  (Q.Type...) -> Void,
                                  (Array<Q>) -> Void,
                                  (Array<Array<Q>?>) -> Void,
                                  (() -> Q?) -> Void
                                 ) -> [String : () -> (Q, Q)] { get }

  var contravariantSelfProp1: (Self?) -> Void { get }
  var contravariantSelfProp2: (() -> Self) -> Void { get }
  var contravariantSelfProp3: (Array<() -> Self>) -> Void { get }
  var contravariantSelfProp4: ([String : () -> Self]) -> Void { get }
  var contravariantSelfProp5: (() -> (Self, Self)) -> Void { get }
  var contravariantSelfProp6: (((Self) -> Void) -> Void) -> Void { get }
  var contravariantSelfProp7: (Self) -> Void { get }
  var contravariantSelfProp8: Array<((Self) -> Void)?> { get }
  var contravariantSelfProp9: ([String : (() -> Self)?]) -> Void { get }
  var contravariantSelfProp10: (Array<[String : Self??]>) -> Void { get }
  var contravariantSelfProp11: (Self.Type) -> Void { get }
  var contravariantAssocProp1: (Q?) -> Void { get }
  var contravariantAssocProp2: (() -> Q) -> Void { get }
  var contravariantAssocProp3: (Array<() -> Q>) -> Void { get }
  var contravariantAssocProp4: ([String : () -> Q]) -> Void { get }
  var contravariantAssocProp5: (() -> (Q, Q)) -> Void { get }
  var contravariantAssocProp6: (((Q) -> Void) -> Void) -> Void { get }
  var contravariantAssocProp7: (Q) -> Void { get }
  var contravariantAssocProp8: Array<((Q) -> Void)?> { get }
  var contravariantAssocProp9: ([String : (() -> Q)?]) -> Void { get }
  var contravariantAssocProp10: (Array<[String : Q??]>) -> Void { get }
  var contravariantAssocProp11: (Q.Type) -> Void { get }

  var invariantSelfProp1: (inout Self) -> Void { get }
  var invariantSelfProp2: ((inout Self) -> Void) -> Void { get }
  var invariantSelfProp3: (inout Array<() -> Self>) -> Void { get }
  var invariantSelfProp4: (Struct<Self>) -> Void { get }
  var invariantSelfProp5: Struct<Self> { get }
  var invariantSelfProp6: Struct<Self>.Inner { get }
  var invariantSelfProp7: ((Struct<Self>) -> Void) -> Void { get }
  var invariantSelfProp8: (Struct<(Self) -> Void>) -> Void { get }
  var invariantSelfProp9: (Struct<() -> Self>) -> Void { get }
  var invariantSelfProp10: (any P1 & Class<Self>) -> Void { get }
  var invariantSelfProp11: Struct<Self>.InnerGeneric<Void> { get }
  var invariantAssocProp1: (inout Q) -> Void { get }
  var invariantAssocProp2: ((inout Q) -> Void) -> Void { get }
  var invariantAssocProp3: (inout Array<() -> Q>) -> Void { get }
  var invariantAssocProp4: (Struct<Q>) -> Void { get }
  var invariantAssocProp5: Struct<Q> { get }
  var invariantAssocProp6: Struct<Q>.Inner { get }
  var invariantAssocProp7: ((Struct<Q>) -> Void) { get }
  var invariantAssocProp8: (Struct<(Q) -> Void>) { get }
  var invariantAssocProp9: (Struct<() -> Q>) -> Void { get }
  var invariantAssocProp10: (any P1 & Class<Q>) -> Void { get }
  var invariantAssocProp11: Struct<Q>.InnerGeneric<Void> { get }

  // Subscripts
  subscript(covariantSelfSubscript1 _: Void) -> Self { get }
  subscript(covariantSelfSubscript2 _: Void) -> Self? { get }
  subscript(covariantSelfSubscript3 _: Void) -> Self.Type { get }
  subscript(covariantSelfSubscript4 _: Void) -> (Self, Self) { get }
  subscript(covariantSelfSubscript5 _: Void) -> Array<Self> { get }
  subscript(covariantSelfSubscript6 _: Void) -> [String : Self] { get }
  subscript(covariantSelfSubscript7 _: (Self) -> Void) -> Self { get }
  subscript(covariantSelfSubscript8 _: (Self...) -> Void) -> Self { get }
  subscript(covariantSelfSubscriptComplex
            _: (Self.Type) -> Void,
            _: (Self.Type...) -> Void,
            _: (Array<Self>) -> Void,
            _: (Array<Array<Self>?>) -> Void,
            _: (() -> Self?) -> Void
            ) -> [String : () -> (Self, Self)] { get }
  subscript(covariantAssocSubscript1 _: Void) -> Q { get }
  subscript(covariantAssocSubscript2 _: Void) -> Q? { get }
  subscript(covariantAssocSubscript3 _: Void) -> Q.Type { get }
  subscript(covariantAssocSubscript4 _: Void) -> (Q, Q) { get }
  subscript(covariantAssocSubscript5 _: Void) -> Array<Q> { get }
  subscript(covariantAssocSubscript6 _: Void) -> [String : Q] { get }
  subscript(covariantAssocSubscript7 _: (Q) -> Void) -> Q { get }
  subscript(covariantAssocSubscript8 _: (Q...) -> Void) -> Self { get }
  subscript(covariantAssocSubscriptComplex
            _: (Q.Type) -> Void,
            _: (Q.Type...) -> Void,
            _: (Array<Q>) -> Void,
            _: (Array<Array<Q>?>) -> Void,
            _: (() -> Q?) -> Void
            ) -> [String : () -> (Q, Q)] { get }

  subscript(contravariantSelfSubscript1 _: Self?) -> Void { get }
  subscript(contravariantSelfSubscript2 _: () -> Self) -> Void { get }
  subscript(contravariantSelfSubscript3 _: Array<() -> Self>) -> Void { get }
  subscript(contravariantSelfSubscript4 _: [String : () -> Self]) -> Void { get }
  subscript(contravariantSelfSubscript5 _: () -> (Self, Self)) -> Void { get }
  subscript(contravariantSelfSubscript6 _: ((Self) -> Void) -> Void) -> Void { get }
  subscript(contravariantSelfSubscript7 _: Void) -> (Self) -> Void { get }
  subscript(contravariantSelfSubscript8 _: Void) -> Array<((Self) -> Void)?> { get }
  subscript(contravariantSelfSubscript9 _: [String : (() -> Self)?]) -> Void { get }
  subscript(contravariantSelfSubscript10 _: Void) -> (Array<[String : Self??]>) -> Void { get }
  subscript(contravariantSelfSubscript11 _: Self.Type) -> Void { get }
  subscript(contravariantAssocSubscript1 _: Q?) -> Void { get }
  subscript(contravariantAssocSubscript2 _: () -> Q) -> Void { get }
  subscript(contravariantAssocSubscript3 _: Array<() -> Q>) -> Void { get }
  subscript(contravariantAssocSubscript4 _: [String : () -> Q]) -> Void { get }
  subscript(contravariantAssocSubscript5 _: () -> (Q, Q)) -> Void { get }
  subscript(contravariantAssocSubscript6 _: ((Q) -> Void) -> Void) -> Void { get }
  subscript(contravariantAssocSubscript7 _: Void) -> (Q) -> Void { get }
  subscript(contravariantAssocSubscript8 _: Void) -> Array<((Q) -> Void)?> { get }
  subscript(contravariantAssocSubscript9 _: [String : (() -> Q)?]) -> Void { get }
  subscript(contravariantAssocSubscript10 _: Void) -> (Array<[String : Q??]>) -> Void { get }
  subscript(contravariantAssocSubscript11 _: Q.Type) -> Void { get }

  subscript(invariantSelfSubscript1 _: Struct<Self>) -> Void { get }
  subscript(invariantSelfSubscript2 _: Void) -> Struct<Self> { get }
  subscript(invariantSelfSubscript3 _: Void) -> Struct<Self>.Inner { get }
  subscript(invariantSelfSubscript4 _: (Struct<Self>) -> Void) -> Void { get }
  subscript(invariantSelfSubscript5 _: Struct<(Self) -> Void>) -> Void { get }
  subscript(invariantSelfSubscript6 _: Struct<() -> Self>) -> Void { get }
  subscript(invariantSelfSubscript7 _: any P1 & Class<Self>) -> Void { get }
  subscript(invariantSelfSubscript8 _: Void) -> Struct<Self>.InnerGeneric<Void> { get }
  subscript(invariantAssocSubscript1 _: Struct<Q>) -> Void { get }
  subscript(invariantAssocSubscript2 _: Void) -> Struct<Q> { get }
  subscript(invariantAssocSubscript3 _: Void) -> Struct<Q>.Inner { get }
  subscript(invariantAssocSubscript4 _: (Struct<Q>) -> Void) -> Void { get }
  subscript(invariantAssocSubscript5 _: Struct<(Q) -> Void>) -> Void { get }
  subscript(invariantAssocSubscript6 _: Struct<() -> Q>) -> Void { get }
  subscript(invariantAssocSubscript7 _: any P1 & Class<Q>) -> Void { get }
  subscript(invariantAssocSubscript8 _: Void) -> Struct<Q>.InnerGeneric<Void> { get }
}
extension P1 {
  func opaqueResultTypeMethod() -> some P1 { self }
  var opaqueResultTypeProp: some P1 { self }
  subscript(opaqueResultTypeSubscript _: Bool) -> some P1 { self }
}

do {
  func testP1(arg: any P1) {
    let _: any P1 = arg.covariantSelf1()
    let _: (any P1)? = arg.covariantSelf2()
    let _: any P1.Type = arg.covariantSelf3()
    let _: (any P1, any P1) = arg.covariantSelf4()
    let _: Array<any P1> = arg.covariantSelf5()
    let _: [String : any P1] = arg.covariantSelf6()
    arg.covariantSelf7 { (_: any P1) in }
    arg.covariantSelf8 { (_: any P1...) in }
    let _: [String : () -> (any P1, any P1)] = arg.covariantSelfComplex(
      { (_: any P1.Type) in },
      { (_: any P1.Type...) in },
      { (_: Array<any P1>) in },
      { (_: Array<Array<any P1>?>) in },
      { (_: () -> (any P1)?) in }
    )

    let _: Any = arg.covariantAssoc1()
    let _: Any? = arg.covariantAssoc2()
    let _: Any.Type = arg.covariantAssoc3()
    let _: (Any, Any) = arg.covariantAssoc4()
    let _: Array<Any> = arg.covariantAssoc5()
    let _: [String : Any] = arg.covariantAssoc6()
    arg.covariantAssoc7 { (_: Any) in }
    arg.covariantAssoc8 { (_: Any...) in }
    let _: [String : () -> (Any, Any)] = arg.covariantAssocComplex(
      { (_: Any.Type) in },
      { (_: Any.Type...) in },
      { (_: Array<Any>) in },
      { (_: Array<Array<Any>?>) in },
      { (_: () -> Any?) in }
    )

    let _: any P1 = arg.covariantSelfProp1
    let _: (any P1)? = arg.covariantSelfProp2
    let _: any P1.Type = arg.covariantSelfProp3
    let _: (any P1, any P1) = arg.covariantSelfProp4
    let _: Array<any P1> = arg.covariantSelfProp5
    let _: [String : any P1] = arg.covariantSelfProp6
    let _: ((any P1) -> Void) -> Void = arg.covariantSelfProp7
    let _: ((any P1...) -> Void) -> Void = arg.covariantSelfProp8
    let _: (
      (any P1.Type) -> Void,
      (any P1.Type...) -> Void,
      (Array<any P1>) -> Void,
      (Array<Array<any P1>?>) -> Void,
      (() -> (any P1)?) -> Void
    ) -> [String : () -> (any P1, any P1)] = arg.covariantSelfPropComplex

    let _: Any = arg.covariantAssocProp1
    let _: Any? = arg.covariantAssocProp2
    let _: Any.Type = arg.covariantAssocProp3
    let _: (Any, Any) = arg.covariantAssocProp4
    let _: Array<Any> = arg.covariantAssocProp5
    let _: [String : Any] = arg.covariantAssocProp6
    let _: ((Any) -> Void) -> Void = arg.covariantAssocProp7
    let _: ((Any...) -> Void) -> Void = arg.covariantAssocProp8
    let _: (
      (Any.Type) -> Void,
      (Any.Type...) -> Void,
      (Array<Any>) -> Void,
      (Array<Array<Any>?>) -> Void,
      (() -> Any?) -> Void
    ) -> [String : () -> (Any, Any)] = arg.covariantAssocPropComplex

    let _: any P1 = arg[covariantSelfSubscript1: ()]
    let _: (any P1)? = arg[covariantSelfSubscript2: ()]
    let _: any P1.Type = arg[covariantSelfSubscript3: ()]
    let _: (any P1, any P1) = arg[covariantSelfSubscript4: ()]
    let _: Array<any P1> = arg[covariantSelfSubscript5: ()]
    let _: [String : any P1] = arg[covariantSelfSubscript6: ()]
    let _: any P1 = arg[covariantSelfSubscript7: { (_: any P1) in }]
    let _: any P1 = arg[covariantSelfSubscript8: { (_: any P1...) in }]
    let _: [String : () -> (any P1, any P1)] = arg[
      covariantSelfSubscriptComplex: { (_: any P1.Type) in },
      { (_: any P1.Type...) in },
      { (_: Array<any P1>) in },
      { (_: Array<Array<any P1>?>) in },
      { (_: () -> (any P1)?) in }
    ]

    let _: Any = arg[covariantAssocSubscript1: ()]
    let _: Any? = arg[covariantAssocSubscript2: ()]
    let _: Any.Type = arg[covariantAssocSubscript3: ()]
    let _: (Any, Any) = arg[covariantAssocSubscript4: ()]
    let _: Array<Any> = arg[covariantAssocSubscript5: ()]
    let _: [String : Any] = arg[covariantAssocSubscript6: ()]
    let _: Any = arg[covariantAssocSubscript7: { (_: Any) in }]
    let _: Any = arg[covariantAssocSubscript8: { (_: Any...) in }]
    let _: [String : () -> (Any, Any)] = arg[
      covariantAssocSubscriptComplex: { (_: Any.Type) in },
      { (_: Any.Type...) in },
      { (_: Array<Any>) in },
      { (_: Array<Array<Any>?>) in },
      { (_: () -> Any?) in }
    ]

    let _: any P1 = arg.opaqueResultTypeMethod()
    let _: any P1 = arg.opaqueResultTypeProp
    let _: any P1 = arg[opaqueResultTypeSubscript: true]

    arg.contravariantSelf1(0) // expected-error {{member 'contravariantSelf1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf2(0) // expected-error {{member 'contravariantSelf2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf3(0) // expected-error {{member 'contravariantSelf3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf4(0) // expected-error {{member 'contravariantSelf4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf5(0) // expected-error {{member 'contravariantSelf5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf6(0) // expected-error {{member 'contravariantSelf6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf7() // expected-error {{member 'contravariantSelf7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf8() // expected-error {{member 'contravariantSelf8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf9(0) // expected-error {{member 'contravariantSelf9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf10() // expected-error {{member 'contravariantSelf10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelf11(0) // expected-error {{member 'contravariantSelf11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc1(0) // expected-error {{member 'contravariantAssoc1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc2(0) // expected-error {{member 'contravariantAssoc2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc3(0) // expected-error {{member 'contravariantAssoc3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc4(0) // expected-error {{member 'contravariantAssoc4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc5(0) // expected-error {{member 'contravariantAssoc5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc6(0) // expected-error {{member 'contravariantAssoc6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc7() // expected-error {{member 'contravariantAssoc7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc8() // expected-error {{member 'contravariantAssoc8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc9(0) // expected-error {{member 'contravariantAssoc9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc10() // expected-error {{member 'contravariantAssoc10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssoc11(0) // expected-error {{member 'contravariantAssoc11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}

    arg.invariantSelf1(0) // expected-error {{member 'invariantSelf1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf2(0) // expected-error {{member 'invariantSelf2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf3(0) // expected-error {{member 'invariantSelf3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf4(0) // expected-error {{member 'invariantSelf4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf5() // expected-error {{member 'invariantSelf5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf6() // expected-error {{member 'invariantSelf6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf7(0) // expected-error {{member 'invariantSelf7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf8(0) // expected-error {{member 'invariantSelf8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf9(0) // expected-error {{member 'invariantSelf9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf10(0) // expected-error {{member 'invariantSelf10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelf11() // expected-error {{member 'invariantSelf11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc1(0) // expected-error {{member 'invariantAssoc1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc2(0) // expected-error {{member 'invariantAssoc2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc3(0) // expected-error {{member 'invariantAssoc3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc4(0) // expected-error {{member 'invariantAssoc4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc5() // expected-error {{member 'invariantAssoc5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc6() // expected-error {{member 'invariantAssoc6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc7(0) // expected-error {{member 'invariantAssoc7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc8(0) // expected-error {{member 'invariantAssoc8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc9(0) // expected-error {{member 'invariantAssoc9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc10(0) // expected-error {{member 'invariantAssoc10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssoc11() // expected-error {{member 'invariantAssoc11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}

    arg.contravariantSelfProp1 // expected-error {{member 'contravariantSelfProp1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp2 // expected-error {{member 'contravariantSelfProp2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp3 // expected-error {{member 'contravariantSelfProp3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp4 // expected-error {{member 'contravariantSelfProp4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp5 // expected-error {{member 'contravariantSelfProp5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp6 // expected-error {{member 'contravariantSelfProp6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp7 // expected-error {{member 'contravariantSelfProp7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp8 // expected-error {{member 'contravariantSelfProp8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp9 // expected-error {{member 'contravariantSelfProp9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp10 // expected-error {{member 'contravariantSelfProp10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantSelfProp11 // expected-error {{member 'contravariantSelfProp11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp1 // expected-error {{member 'contravariantAssocProp1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp2 // expected-error {{member 'contravariantAssocProp2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp3 // expected-error {{member 'contravariantAssocProp3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp4 // expected-error {{member 'contravariantAssocProp4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp5 // expected-error {{member 'contravariantAssocProp5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp6 // expected-error {{member 'contravariantAssocProp6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp7 // expected-error {{member 'contravariantAssocProp7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp8 // expected-error {{member 'contravariantAssocProp8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp9 // expected-error {{member 'contravariantAssocProp9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp10 // expected-error {{member 'contravariantAssocProp10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.contravariantAssocProp11 // expected-error {{member 'contravariantAssocProp11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}

    arg.invariantSelfProp1 // expected-error {{member 'invariantSelfProp1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp2 // expected-error {{member 'invariantSelfProp2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp3 // expected-error {{member 'invariantSelfProp3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp4 // expected-error {{member 'invariantSelfProp4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp5 // expected-error {{member 'invariantSelfProp5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp6 // expected-error {{member 'invariantSelfProp6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp7 // expected-error {{member 'invariantSelfProp7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp8 // expected-error {{member 'invariantSelfProp8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp9 // expected-error {{member 'invariantSelfProp9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp10 // expected-error {{member 'invariantSelfProp10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantSelfProp11 // expected-error {{member 'invariantSelfProp11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp1 // expected-error {{member 'invariantAssocProp1' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp2 // expected-error {{member 'invariantAssocProp2' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp3 // expected-error {{member 'invariantAssocProp3' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp4 // expected-error {{member 'invariantAssocProp4' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp5 // expected-error {{member 'invariantAssocProp5' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp6 // expected-error {{member 'invariantAssocProp6' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp7 // expected-error {{member 'invariantAssocProp7' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp8 // expected-error {{member 'invariantAssocProp8' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp9 // expected-error {{member 'invariantAssocProp9' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp10 // expected-error {{member 'invariantAssocProp10' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg.invariantAssocProp11 // expected-error {{member 'invariantAssocProp11' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}

    arg[contravariantSelfSubscript1: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript2: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript3: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript4: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript5: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript6: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript7: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript8: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript9: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript10: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantSelfSubscript11: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript1: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript2: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript3: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript4: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript5: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript6: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript7: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript8: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript9: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript10: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[contravariantAssocSubscript11: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}

    arg[invariantSelfSubscript1: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript2: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript3: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript4: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript5: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript6: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript7: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantSelfSubscript8: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript1: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript2: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript3: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript4: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript5: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript6: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript7: 0] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
    arg[invariantAssocSubscript8: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1'; consider using a generic constraint instead}}
  }
}

protocol P1_TypeMemberOnInstanceAndViceVersa {
  static func static_covariantSelfMethod() -> Self
  static var static_covariantSelfProp: Self { get }
  static subscript(static_covariantSelfSubscript _: Void) -> Self { get }

  static func static_invariantSelfMethod() -> Struct<Self>
  static var static_invariantSelfProp: Struct<Self> { get }
  static subscript(static_invariantSelfSubscript _: Void) -> Struct<Self> { get }

  func covariantSelfMethod() -> Self

  func invariantSelfMethod() -> Struct<Self>
  var invariantSelfProp: Struct<Self> { get }
  subscript(invariantSelfSubscript _: Void) -> Struct<Self> { get }
}
do {
  // Test that invalid reference errors prevail over unsupported existential
  // member accesses.
  func test(protoMeta: (any P1_TypeMemberOnInstanceAndViceVersa).Type,
            existMeta: any P1_TypeMemberOnInstanceAndViceVersa.Type,
            instance: any P1_TypeMemberOnInstanceAndViceVersa) {
    // P1_TypeMemberOnInstanceAndViceVersa.Protocol
    protoMeta.static_invariantSelfMethod() // expected-error {{static member 'static_invariantSelfMethod' cannot be used on protocol metatype '(any P1_TypeMemberOnInstanceAndViceVersa).Type'}}
    protoMeta.static_invariantSelfProp // expected-error {{static member 'static_invariantSelfProp' cannot be used on protocol metatype '(any P1_TypeMemberOnInstanceAndViceVersa).Type'}}
    protoMeta[static_invariantSelfSubscript: ()] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P1_TypeMemberOnInstanceAndViceVersa).Type'}}
    _ = protoMeta.covariantSelfMethod // ok
    protoMeta.invariantSelfMethod // expected-error {{member 'invariantSelfMethod' cannot be used on value of type '(any P1_TypeMemberOnInstanceAndViceVersa).Type'; consider using a generic constraint instead}}
    protoMeta.invariantSelfProp // expected-error {{instance member 'invariantSelfProp' cannot be used on type 'any P1_TypeMemberOnInstanceAndViceVersa'}}
    protoMeta[invariantSelfSubscript: ()] // expected-error {{instance member 'subscript' cannot be used on type 'any P1_TypeMemberOnInstanceAndViceVersa'}}

    // P1_TypeMemberOnInstanceAndViceVersa.Type
    _ = existMeta.static_covariantSelfMethod // ok
    _ = existMeta.static_covariantSelfProp // ok
    _ = existMeta[static_covariantSelfSubscript: ()] // ok
    existMeta.static_invariantSelfMethod // expected-error {{member 'static_invariantSelfMethod' cannot be used on value of type 'any P1_TypeMemberOnInstanceAndViceVersa.Type'; consider using a generic constraint instead}}
    existMeta.static_invariantSelfProp // expected-error {{member 'static_invariantSelfProp' cannot be used on value of type 'any P1_TypeMemberOnInstanceAndViceVersa.Type'; consider using a generic constraint instead}}
    existMeta[static_invariantSelfSubscript: ()] // expected-error {{member 'subscript' cannot be used on value of type 'any P1_TypeMemberOnInstanceAndViceVersa.Type'; consider using a generic constraint instead}}
    existMeta.invariantSelfMethod // expected-error {{instance member 'invariantSelfMethod' cannot be used on type 'P1_TypeMemberOnInstanceAndViceVersa'}}
    existMeta.invariantSelfProp // expected-error {{instance member 'invariantSelfProp' cannot be used on type 'P1_TypeMemberOnInstanceAndViceVersa'}}
    existMeta[invariantSelfSubscript: ()] // expected-error {{instance member 'subscript' cannot be used on type 'P1_TypeMemberOnInstanceAndViceVersa'}}

    // P1_TypeMemberOnInstanceAndViceVersa
    instance.static_invariantSelfMethod // expected-error {{static member 'static_invariantSelfMethod' cannot be used on instance of type 'any P1_TypeMemberOnInstanceAndViceVersa'}}
    instance.static_invariantSelfProp // expected-error {{static member 'static_invariantSelfProp' cannot be used on instance of type 'any P1_TypeMemberOnInstanceAndViceVersa'}}
    instance[static_invariantSelfSubscript: ()] // expected-error {{static member 'subscript' cannot be used on instance of type 'any P1_TypeMemberOnInstanceAndViceVersa'}}
  }
}

// A protocol member accessed with an existential value might have generic
// constraints that require the ability to spell an opened archetype in order
// to be satisfied. Such are
// - superclass requirements, when the object is a non-'Self'-rooted type
//   parameter, and the subject is dependent on 'Self', e.g. U : G<Self.A>
// - same-type requirements, when one side is dependent on 'Self', and the
//   other is a non-'Self'-rooted type parameter, e.g. U.Element == Self.
//
// Because opened archetypes are not part of the surface language, these
// constraints render the member inaccessible.
//
// Note: 'Self'-rooted type parameters that are invalid in the context of the
// existential base type are ignored -- the underlying requirement failure is
// considered a more pressing issue.
protocol UnfulfillableGenericRequirements {
  associatedtype A
}
extension UnfulfillableGenericRequirements {
  func method1() where A : Class<Self> {}
  func method2() where A: Sequence, A.Element == Self {}
  func method3<U>(_: U) -> U {}
  func method4<U>(_: U) where U : Class<Self.A> {}
  // expected-note@-1 3 {{where 'U' = 'Bool'}}
  func method5<U>(_: U) where U: Sequence, Self == U.Element {}
  // expected-note@-1 {{where 'U' = 'Bool'}}

  // expected-note@+1 2 {{where 'U' = 'Bool'}}
  func method6<U>(_: U) where U: UnfulfillableGenericRequirements,
                              A: Sequence, A.Element: Sequence,
                              U.A == A.Element.Element {}
  func method7<U>(_: U) where U: UnfulfillableGenericRequirements & Class<Self> {}

  func method8<U>(_: U) where U == Self.A {}
}
do {
  let exist: any UnfulfillableGenericRequirements

  exist.method1() // expected-error {{instance method 'method1()' requires that 'Self.A' inherit from 'Class<Self>'}}
  exist.method2()
  // expected-error@-1 {{instance method 'method2()' requires the types 'Self' and 'Self.A.Element' be equivalent}}
  // expected-error@-2 {{instance method 'method2()' requires that 'Self.A' conform to 'Sequence'}}
  _ = exist.method3(false) // ok
  exist.method4(false)
  // expected-error@-1 {{instance method 'method4' requires that 'Bool' inherit from 'Class<Self.A>'}}
  // expected-error@-2 {{member 'method4' cannot be used on value of type 'any UnfulfillableGenericRequirements'; consider using a generic constraint instead}}
  exist.method5(false)
  // expected-error@-1 {{instance method 'method5' requires that 'Bool' conform to 'Sequence'}}
  // expected-error@-2 {{member 'method5' cannot be used on value of type 'any UnfulfillableGenericRequirements'; consider using a generic constraint instead}}

  exist.method7(false)
  // expected-error@-1 {{instance method 'method7' requires that 'U' conform to 'UnfulfillableGenericRequirements'}}
  // expected-error@-2 {{member 'method7' cannot be used on value of type 'any UnfulfillableGenericRequirements'; consider using a generic constraint instead}}

  exist.method8(false)
  // expected-error@-1 {{member 'method8' cannot be used on value of type 'any UnfulfillableGenericRequirements'; consider using a generic constraint instead}}
}

// Make sure this also works in a generic context!
struct G<X, Y, Z> {
  func doIt() {
    let exist: any UnfulfillableGenericRequirements

    exist.method8(false)
    // expected-error@-1 {{member 'method8' cannot be used on value of type 'any UnfulfillableGenericRequirements'; consider using a generic constraint instead}}
  }
}
protocol UnfulfillableGenericRequirementsDerived1: UnfulfillableGenericRequirements where A == Bool {}
protocol UnfulfillableGenericRequirementsDerived2: UnfulfillableGenericRequirements where A == Class<Self> {}
do {
  // Test that 'Self' dependencies are computed relative to the base type.
  let exist1: any UnfulfillableGenericRequirementsDerived1
  let exist2: any UnfulfillableGenericRequirementsDerived2

  exist1.method4(false)
  // expected-error@-1 {{instance method 'method4' requires that 'Bool' inherit from 'Class<Self.A>}}
  exist2.method4(false)
  // expected-error@-1 {{member 'method4' cannot be used on value of type 'any UnfulfillableGenericRequirementsDerived2'; consider using a generic constraint instead}}
  // expected-error@-2 {{instance method 'method4' requires that 'Bool' inherit from 'Class<Self.A>'}}
}
protocol UnfulfillableGenericRequirementsDerived3: UnfulfillableGenericRequirements where A: Sequence, A.Element: Sequence {}
do {
  // Test that 'Self'-rooted type parameters that are invalid in the context of
  // the existential base type are ignored.
  let exist1: any UnfulfillableGenericRequirements
  let exist2: any UnfulfillableGenericRequirementsDerived3

  exist1.method6(false)
  // expected-error@-1 {{instance method 'method6' requires that 'Self.A.Element' conform to 'Sequence'}}
  // expected-error@-2 {{instance method 'method6' requires that 'Self.A' conform to 'Sequence'}}
  // expected-error@-3 {{instance method 'method6' requires that 'Bool' conform to 'UnfulfillableGenericRequirements'}}
  exist2.method6(false)
  // expected-error@-1 {{member 'method6' cannot be used on value of type 'any UnfulfillableGenericRequirementsDerived3'; consider using a generic constraint instead}}
  // expected-error@-2 {{instance method 'method6' requires that 'Bool' conform to 'UnfulfillableGenericRequirements'}}
}

// Test that we don't determine existential availability based on type
// parameters that are invalid in the context of the existential base type --
// the requirement failure is a more pressing issue.
protocol InvalidTypeParameters {
  associatedtype A
}
extension InvalidTypeParameters {
  func method1() -> A.A where A: InvalidTypeParameters {}
  func method2(_: A.A) where A: InvalidTypeParameters {}
  func method3(_: A.A, _: A) where A: InvalidTypeParameters {}
}
do {
  let exist: any InvalidTypeParameters

  exist.method1() // expected-error {{instance method 'method1()' requires that 'Self.A' conform to 'InvalidTypeParameters'}}
  exist.method2(false) // expected-error {{instance method 'method2' requires that 'Self.A' conform to 'InvalidTypeParameters'}}
  exist.method3(false, false) // expected-error {{instance method 'method3' requires that 'Self.A' conform to 'InvalidTypeParameters'}}
  // expected-error@-1 {{member 'method3' cannot be used on value of type 'any InvalidTypeParameters'; consider using a generic constraint instead}}
}

protocol GenericRequirementFailures {
  associatedtype A
}
extension GenericRequirementFailures where A == Never {
  func method1() {}
  func method2() -> Self {}
  func method3(_: A) {}
}
extension GenericRequirementFailures where A: GenericRequirementFailures {
  func method4() {}
}
do {
  let exist: any GenericRequirementFailures

  exist.method1() // expected-error {{referencing instance method 'method1()' on 'GenericRequirementFailures' requires the types 'Self.A' and 'Never' be equivalent}}
  exist.method2() // expected-error {{referencing instance method 'method2()' on 'GenericRequirementFailures' requires the types 'Self.A' and 'Never' be equivalent}}
  exist.method3(false) // expected-error {{referencing instance method 'method3' on 'GenericRequirementFailures' requires the types 'Self.A' and 'Never' be equivalent}}
  // expected-error@-1 {{member 'method3' cannot be used on value of type 'any GenericRequirementFailures'; consider using a generic constraint instead}}
  exist.method4() // expected-error {{referencing instance method 'method4()' on 'GenericRequirementFailures' requires that 'Self.A' conform to 'GenericRequirementFailures'}}
}
protocol GenericRequirementFailuresDerived: GenericRequirementFailures where A: GenericRequirementFailures {}
do {
  let exist: any GenericRequirementFailuresDerived
  exist.method4() // ok

}

// Settable storage members with a 'Self' result type may not be used with an
// existential base.
protocol P2 {
  subscript() -> Self { get set }

  var prop: Self { get set }
}
func takesP2(p2: any P2) {
  _ = p2[]
  // expected-error@-1{{member 'subscript' cannot be used on value of type 'any P2'; consider using a generic constraint instead}}
  _ = p2.prop
  // expected-error@-1{{member 'prop' cannot be used on value of type 'any P2'; consider using a generic constraint instead}}
}

protocol MiscTestsProto {
  associatedtype R : IteratorProtocol, Sequence
  func getR() -> R

  associatedtype Assoc
  subscript() -> Assoc { get }
  var getAssoc: Assoc? { get }
}
do {
  func miscTests(_ arg: any MiscTestsProto) {
    var r: any Sequence & IteratorProtocol = arg.getR()
    r.makeIterator() // expected-error {{inferred result type 'any IteratorProtocol' requires explicit coercion due to loss of generic requirements}} {{19-19=as any IteratorProtocol}}
    r.next() // expected-warning {{result of call to 'next()' is unused}}
    r.nonexistent() // expected-error {{value of type 'any IteratorProtocol & Sequence' has no member 'nonexistent'}}

    arg[] // expected-warning {{expression of type 'Any' is unused}}
    arg.getAssoc // expected-warning {{expression of type 'Any?' is unused}}
  }
}

// Test that covariant erasure turns metatypes into existential metatypes.
protocol CovariantMetatypes {
  associatedtype Q

  func covariantSelfMetatype1(_: (Self.Type.Type.Type) -> Void)
  func covariantSelfMetatype2() -> (Self.Type, Self.Type.Type)

  func covariantAssocMetatype1(_: (Q.Type.Type.Type) -> Void)
  func covariantAssocMetatype2() -> (Q.Type, Q.Type.Type)

  var covariantSelfMetatypeProp1: Self.Type.Type.Type { get }
  var covariantSelfMetatypeProp2: (Self.Type, Self.Type.Type) { get }

  var covariantAssocMetatypeProp1: Q.Type.Type.Type { get }
  var covariantAssocMetatypeProp2: (Q.Type, Q.Type.Type) { get }

  subscript(covariantSelfMetatypeSubscript1 _: (Self.Type.Type.Type) -> Void) -> Self.Type { get }
  subscript(covariantSelfMetatypeSubscript2 _: Void) -> (Self.Type, Self.Type.Type) { get }

  subscript(covariantAssocMetatypeSubscript1 _: (Q.Type.Type.Type) -> Void) -> Q.Type { get }
  subscript(covariantAssocMetatypeSubscript2 _: Void) -> (Q.Type, Q.Type.Type) { get }
}
do {
  func testCovariantMetatypes(arg: any CovariantMetatypes) {
    arg.covariantSelfMetatype1 { (_: any CovariantMetatypes.Type.Type.Type) in }
    let _: (any CovariantMetatypes.Type, any CovariantMetatypes.Type.Type) = arg.covariantSelfMetatype2()

    arg.covariantAssocMetatype1 { (_: Any.Type.Type.Type) in }
    let _: (Any.Type, Any.Type.Type) = arg.covariantAssocMetatype2()

    let _: any CovariantMetatypes.Type.Type.Type = arg.covariantSelfMetatypeProp1
    let _: (any CovariantMetatypes.Type, any CovariantMetatypes.Type.Type) = arg.covariantSelfMetatypeProp2

    let _: Any.Type.Type.Type = arg.covariantAssocMetatypeProp1
    let _: (Any.Type, Any.Type.Type) = arg.covariantAssocMetatypeProp2

    let _: any CovariantMetatypes.Type = arg[covariantSelfMetatypeSubscript1: { (_: any CovariantMetatypes.Type.Type.Type) in }]
    let _: (any CovariantMetatypes.Type, any CovariantMetatypes.Type.Type) = arg[covariantSelfMetatypeSubscript2: ()]

    let _: Any.Type = arg[covariantAssocMetatypeSubscript1: { (_: Any.Type.Type.Type) in }]
    let _: (Any.Type, Any.Type.Type) = arg[covariantAssocMetatypeSubscript2: ()]
  }
}

// Test that a reference to a 'Self'-rooted dependent member type does not
// affect the ability to reference a protocol member on an existential when
// it is *fully* concrete.
protocol ConcreteAssocTypes {
  associatedtype A1 where A1 == Struct<Self>
  associatedtype A2 where A2 == (Bool, Self)
  associatedtype A3 where A3 == any Class<A4> & ConcreteAssocTypes
  associatedtype A4

  func method1(_: A1)
  func method2() -> Struct<A2>
  func method3(_: A3)

  var property1: A1 { get }
  var property2: A2 { get }
  var property3: A3 { get }

  subscript(subscript1 _: A3) -> Bool { get }
  subscript(subscript2 _: Bool) -> A1 { get }
  subscript(subscript3 _: A2) -> Bool { get }

  associatedtype A5 where A5 == Bool
  associatedtype A6 where A6 == any ConcreteAssocTypes
  associatedtype A7 where A7 == A8.A5
  associatedtype A8: ConcreteAssocTypes

  func method4(_: Struct<A5>, _: A6.Type, _: () -> A5) -> any Class<Struct<A7>.Inner> & ConcreteAssocTypes

  var property4: (Struct<A5>, A6.Type, () -> A5) -> any Class<Struct<A7>.Inner> & ConcreteAssocTypes { get }

  subscript(subscript4 _: Struct<A5>, _: A6.Type, _: () -> A5) -> any Class<Struct<A7>.Inner> & ConcreteAssocTypes { get }
}
do {
  func test(arg: any ConcreteAssocTypes) {
    _ = arg.method1 // expected-error {{member 'method1' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg.method2 // expected-error {{member 'method2' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg.method3 // expected-error {{member 'method3' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg.property1 // expected-error {{member 'property1' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    // Covariant 'Self' erasure works in conjunction with concrete associated types.
    let _: (Bool, any ConcreteAssocTypes) = arg.property2 // ok
    _ = arg.property3 // expected-error {{member 'property3' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg[subscript1: false] // expected-error {{member 'subscript' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg[subscript2: false] // expected-error {{member 'subscript' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}
    _ = arg[subscript3: false] // expected-error {{member 'subscript' cannot be used on value of type 'any ConcreteAssocTypes'; consider using a generic constraint instead}}

    let _: (
      Struct<Bool>, (any ConcreteAssocTypes).Type, () -> Bool
    ) -> any Class<Struct<Bool>.Inner> & ConcreteAssocTypes = arg.method4

    let _: (
      Struct<Bool>, (any ConcreteAssocTypes).Type, () -> Bool
    ) -> any Class<Struct<Bool>.Inner> & ConcreteAssocTypes = arg.property4

    let _: any Class<Struct<Bool>.Inner> & ConcreteAssocTypes =
      arg[
        subscript4: Struct<Bool>(), (any ConcreteAssocTypes).self, { true }
      ]
  }
}

protocol ConcreteAssocTypeComposition1 {
  associatedtype A
  func method(_: A)
}
protocol ConcreteAssocTypeComposition2 where A == Bool {
  associatedtype A
}
do {
  let exist: any ConcreteAssocTypeComposition1 & ConcreteAssocTypeComposition2
  exist.method(true) // ok
}

// Edge case: an associated type can be resolved through a class conformance.
class Class1Simple: ConcreteAssocTypeThroughClass {
  typealias A = Bool
}
class Class1Generic<A>: ConcreteAssocTypeThroughClass {
}
protocol ConcreteAssocTypeThroughClass {
  associatedtype A
}
protocol ConcreteAssocTypeThroughClassRefined: ConcreteAssocTypeThroughClass {
  func method(_: A)
}
extension ConcreteAssocTypeThroughClassRefined {
  func test(arg1: any ConcreteAssocTypeThroughClassRefined & Class1Generic<Self>,
            arg2: any ConcreteAssocTypeThroughClassRefined & Class1Simple) {
    arg1.method(self) // ok
    arg2.method(true) // ok
  }
}

protocol ConcreteAssocTypeCollision1 where A == Bool {
  associatedtype A
  func method(_: A)
}
protocol ConcreteAssocTypeCollision2 where A == Never {
  associatedtype A
}
do {
  let exist: any ConcreteAssocTypeCollision1 & ConcreteAssocTypeCollision2
  // FIXME: Should 'A' be ambiguous here?
  exist.method(true)
}

class BadConformanceClass: CompositionBrokenClassConformance_a {}
// expected-error@-1 {{type 'BadConformanceClass' does not conform to protocol 'CompositionBrokenClassConformance_a'}}
protocol CompositionBrokenClassConformance_a {
  associatedtype A // expected-note {{protocol requires nested type 'A'; do you want to add it?}}
}
protocol CompositionBrokenClassConformance_b: CompositionBrokenClassConformance_a {
  func method(_: A)
}
do {
  // FIXME: Should GenericSignature::getConcreteType return the null type instead
  // of the error type here for Self.A, despite the broken conformance?
  let exist: any CompositionBrokenClassConformance_b & BadConformanceClass
  exist.method(false) // expected-error {{type of expression is ambiguous without more context}}
}

/// Covariant Associated Type Erasure

class Class2Base {}
class Class2Derived<T>: Class2Base {}
protocol CovariantAssocTypeErasure {
  associatedtype A1
  associatedtype A2: AnyObject
  associatedtype A3: CovariantAssocTypeErasure
  associatedtype A4: Class2Base
  associatedtype A5: Class2Derived<Self>
  associatedtype A6: CovariantAssocTypeErasure & Class2Base
  associatedtype A7: Class2Derived<Self> & CovariantAssocTypeErasure

  associatedtype B1 where B1 == Optional<A1>
  associatedtype B2 where B2 == (A2, Bool)
  associatedtype B3 where B3 == A3.Type
  associatedtype B4 where B4 == Array<A4>
  associatedtype B5 where B5 == Dictionary<String, A5>

  func method1() -> A1
  func method2() -> A2
  func method3() -> A3
  func method4() -> A4
  func method5() -> A5
  func method6() -> A6
  func method7() -> A7

  func method8() -> B1
  func method9() -> B2
  func method10() -> B3
  func method11() -> B4
  func method12() -> B5
}
protocol CovariantAssocTypeErasureDerived: CovariantAssocTypeErasure
where A1: CovariantAssocTypeErasureDerived,
      A2: Class2Base,
      A3: CovariantAssocTypeErasureDerived,
      A4: CovariantAssocTypeErasureDerived,
      A5: CovariantAssocTypeErasureDerived,
      A6: CovariantAssocTypeErasureDerived,
      A7: Sequence {}
do {
  let exist: any CovariantAssocTypeErasure

  let _: Any = exist.method1()
  let _: AnyObject = exist.method2()
  let _: any CovariantAssocTypeErasure = exist.method3()
  let _: Class2Base = exist.method4()
  let _: Class2Base = exist.method5()
  let _: any Class2Base & CovariantAssocTypeErasure = exist.method6()
  let _: any Class2Base & CovariantAssocTypeErasure = exist.method7()
  let _: Any? = exist.method8()
  let _: (AnyObject, Bool) = exist.method9()
  let _: any CovariantAssocTypeErasure.Type = exist.method10()
  let _: Array<Class2Base> = exist.method11()
  let _: Dictionary<String, Class2Base> = exist.method12()

  let _ = exist.method1()
  let _ = exist.method2()
  let _ = exist.method3()
  let _ = exist.method4()
  let _ = exist.method5()
  let _ = exist.method6()
  let _ = exist.method7()
  let _ = exist.method8()
  let _ = exist.method9()
  let _ = exist.method10()
  let _ = exist.method11()
  let _ = exist.method12()
}
do {
  let exist: any CovariantAssocTypeErasureDerived

  let _: any CovariantAssocTypeErasureDerived = exist.method1()
  let _: Class2Base = exist.method2()
  let _: any CovariantAssocTypeErasureDerived = exist.method3()
  let _: any Class2Base & CovariantAssocTypeErasureDerived = exist.method4()
  let _: any Class2Base & CovariantAssocTypeErasureDerived = exist.method5()
  let _: any Class2Base & CovariantAssocTypeErasureDerived = exist.method6()
  let _: any Class2Base & CovariantAssocTypeErasure & Sequence = exist.method7()

  let _: (any CovariantAssocTypeErasureDerived)? = exist.method8()
  let _: (Class2Base, Bool) = exist.method9()
  let _: any CovariantAssocTypeErasureDerived.Type = exist.method10()
  let _: Array<any Class2Base & CovariantAssocTypeErasureDerived> = exist.method11()
  let _: Dictionary<String, any Class2Base & CovariantAssocTypeErasureDerived> = exist.method12()
}
