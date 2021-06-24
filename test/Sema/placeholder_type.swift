// RUN: %target-typecheck-verify-swift

let x: _ = 0 // expected-error {{placeholders are not allowed as top-level types}}
let x2 = x
let dict1: [_: Int] = ["hi": 0]
let dict2: [Character: _] = ["h": 0]

let arr = [_](repeating: "hi", count: 3)

func foo(_ arr: [_] = [0]) {} // expected-error {{placeholder type not allowed here}}

let foo = _.foo // expected-error {{placeholders are not allowed as top-level types}}
let zero: _ = .zero // expected-error {{placeholders are not allowed as top-level types}}

struct S<T> {
    var x: T
}

var s1: S<_> = .init(x: 0)
var s2 = S<_>(x: 0)

let losslessStringConverter = Double.init as (String) -> _?

let optInt: _? = 0
let implicitOptInt: _! = 0

let func1: (_) -> Double = { (x: Int) in 0.0 }
let func2: (Int) -> _ = { x in 0.0 }
let func3: (_) -> _ = { (x: Int) in 0.0 }
let func4: (_, String) -> _ = { (x: Int, y: String) in 0.0 }
let func5: (_, String) -> _ = { (x: Int, y: Double) in 0.0 } // expected-error {{cannot convert value of type '(Int, Double) -> Double' to specified type '(_, String) -> _'}}

let type: _.Type = Int.self
let type2: Int.Type.Type = _.Type.self

struct MyType1<T, U> {
    init(t: T, mt2: MyType2<T>) where U == MyType2<T> {}
}

struct MyType2<T> {
    init(t: T) {}
}

let _: MyType2<_> = .init(t: "c" as Character)
let _: MyType1<_, MyType2<_>> = .init(t: "s" as Character, mt2: .init(t: "c" as Character))

func dictionary<K, V>(ofType: [K: V].Type) -> [K: V] { [:] }

let _: [String: _] = dictionary(ofType: [_: Int].self)
let _: [_: _] = dictionary(ofType: [String: Int].self)
let _: [String: Int] = dictionary(ofType: _.self) // expected-error {{placeholders are not allowed as top-level types}}

let _: @convention(c) _ = { 0 } // expected-error {{@convention attribute only applies to function types}}
let _: @convention(c) (_) -> _ = { (x: Double) in 0 }
let _: @convention(c) (_) -> Int = { (x: Double) in 0 }

struct NonObjc {}

let _: @convention(c) (_) -> Int = { (x: NonObjc) in 0 } // expected-error {{'(NonObjc) -> Int' is not representable in Objective-C, so it cannot be used with '@convention(c)'}}

func overload() -> Int? { 0 }
func overload() -> String { "" }

let _: _? = overload()
let _ = overload() as _?

struct Bar<T, U>
where T: ExpressibleByIntegerLiteral, U: ExpressibleByIntegerLiteral {
    var t: T
    var u: U
    func frobnicate() -> Bar {
        return Bar(t: 42, u: 42)
    }
}

extension Bar {
    func frobnicate2() -> Bar<_, _> { // expected-error {{placeholder type not allowed here}}
        return Bar(t: 42, u: 42)
    }
    func frobnicate3() -> Bar {
        return Bar<_, _>(t: 42, u: 42)
    }
    func frobnicate4() -> Bar<_, _> { // expected-error {{placeholder type not allowed here}}
        return Bar<_, _>(t: 42, u: 42)
    }
    func frobnicate5() -> Bar<_, U> { // expected-error {{placeholder type not allowed here}}
        return Bar(t: 42, u: 42)
    }
    func frobnicate6() -> Bar {
        return Bar<_, U>(t: 42, u: 42)
    }
    func frobnicate7() -> Bar<_, _> { // expected-error {{placeholder type not allowed here}}
        return Bar<_, U>(t: 42, u: 42)
    }
    func frobnicate8() -> Bar<_, U> { // expected-error {{placeholder type not allowed here}}
        return Bar<_, _>(t: 42, u: 42)
    }
}

// FIXME: We should probably have better diagnostics for these situations--the user probably meant to use implicit member syntax
let _: Int = _() // expected-error {{placeholders are not allowed as top-level types}}
let _: () -> Int = { _() } // expected-error {{unable to infer closure type in the current context}} expected-error {{placeholders are not allowed as top-level types}}
let _: Int = _.init() // expected-error {{placeholders are not allowed as top-level types}}
let _: () -> Int = { _.init() } // expected-error {{unable to infer closure type in the current context}} expected-error {{placeholders are not allowed as top-level types}}

func returnsInt() -> Int { _() } // expected-error {{placeholders are not allowed as top-level types}}
func returnsIntClosure() -> () -> Int { { _() } } // expected-error {{unable to infer closure type in the current context}} expected-error {{placeholders are not allowed as top-level types}}
func returnsInt2() -> Int { _.init() }  // expected-error {{placeholders are not allowed as top-level types}}
func returnsIntClosure2() -> () -> Int { { _.init() } } // expected-error {{unable to infer closure type in the current context}} expected-error {{placeholders are not allowed as top-level types}}

let _: Int.Type = _ // expected-error {{'_' can only appear in a pattern or on the left side of an assignment}}
let _: Int.Type = _.self // expected-error {{placeholders are not allowed as top-level types}}

struct SomeSuperLongAndComplexType {}
func getSomething() -> SomeSuperLongAndComplexType? { .init() }
let something: _! = getSomething()

extension Array where Element == Int {
    static var staticMember: Self { [] }
    static func staticFunc() -> Self { [] }

    var member: Self { [] }
    func method() -> Self { [] }
}

extension Array {
    static var otherStaticMember: Self { [] }
}

let _ = [_].staticMember
let _ = [_].staticFunc()
let _ = [_].otherStaticMember.member
let _ = [_].otherStaticMember.method()

func f(x: Any, arr: [Int]) {
    // FIXME: Better diagnostics here. Maybe we should suggest replacing placeholders with 'Any'?

    if x is _ {} // expected-error {{placeholders are not allowed as top-level types}}
    if x is [_] {} // expected-error {{type of expression is ambiguous without more context}}
    if x is () -> _ {} // expected-error {{type of expression is ambiguous without more context}}
    if let y = x as? _ {} // expected-error {{placeholders are not allowed as top-level types}}
    if let y = x as? [_] {} // expected-error {{type of expression is ambiguous without more context}}
    if let y = x as? () -> _ {} // expected-error {{type of expression is ambiguous without more context}}
    let y1 = x as! _ // expected-error {{placeholders are not allowed as top-level types}}
    let y2 = x as! [_] // expected-error {{type of expression is ambiguous without more context}}
    let y3 = x as! () -> _ // expected-error {{type of expression is ambiguous without more context}}

    switch x {
    case is _: break // expected-error {{placeholder type not allowed here}}
    case is [_]: break // expected-error {{placeholder type not allowed here}}
    case is () -> _: break // expected-error {{placeholder type not allowed here}}
    case let y as _: break // expected-error {{placeholder type not allowed here}}
    case let y as [_]: break // expected-error {{placeholder type not allowed here}}
    case let y as () -> _: break // expected-error {{placeholder type not allowed here}}
    }

    if arr is _ {} // expected-error {{placeholders are not allowed as top-level types}}
    if arr is [_] {} // expected-error {{type of expression is ambiguous without more context}}
    if arr is () -> _ {} // expected-error {{type of expression is ambiguous without more context}}
    if let y = arr as? _ {} // expected-error {{placeholders are not allowed as top-level types}}
    if let y = arr as? [_] {} // expected-error {{type of expression is ambiguous without more context}}
    if let y = arr as? () -> _ {} // expected-error {{type of expression is ambiguous without more context}}
    let y1 = arr as! _ // expected-error {{placeholders are not allowed as top-level types}}
    let y2 = arr as! [_] // expected-error {{type of expression is ambiguous without more context}}
    let y3 = arr as! () -> _ // expected-error {{type of expression is ambiguous without more context}}

    switch arr {
    case is _: break // expected-error {{placeholder type not allowed here}}
    case is [_]: break // expected-error {{placeholder type not allowed here}}
    case is () -> _: break // expected-error {{placeholder type not allowed here}}
    case let y as _: break // expected-error {{placeholder type not allowed here}}
    case let y as [_]: break // expected-error {{placeholder type not allowed here}}
    case let y as () -> _: break // expected-error {{placeholder type not allowed here}}
    }
}

protocol Publisher {
    associatedtype Output
    associatedtype Failure
}

struct Just<Output>: Publisher {
    typealias Failure = Never
}

struct SetFailureType<Output, Failure>: Publisher {}

extension Publisher {
    func setFailureType<T>(to: T.Type) -> SetFailureType<Output, T> {
        return .init()
    }
}

let _: SetFailureType<Int, String> = Just<Int>().setFailureType(to: _.self) // expected-error {{placeholders are not allowed as top-level types}}
let _: SetFailureType<Int, [String]> = Just<Int>().setFailureType(to: [_].self)
let _: SetFailureType<Int, (String) -> Double> = Just<Int>().setFailureType(to: ((_) -> _).self)
let _: SetFailureType<Int, (String, Double)> = Just<Int>().setFailureType(to: (_, _).self)

// TODO: Better error message here? Would be nice if we could point to the placeholder...
let _: SetFailureType<Int, String> = Just<Int>().setFailureType(to: _.self).setFailureType(to: String.self) // expected-error {{placeholders are not allowed as top-level types}}

_ = (1...10)
    .map {
        (
            $0,
            (
                "\($0)",
                $0 > 5
            )
        )
    }
    .map { (intValue, x: (_, boolValue: _)) in
        x.boolValue ? intValue : 0
    }
