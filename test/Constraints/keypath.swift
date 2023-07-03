// RUN: %target-swift-frontend -typecheck -verify %S/Inputs/keypath.swift -primary-file %s

struct S {
  let i: Int

  init() {
    let _: WritableKeyPath<S, Int> = \.i // no error for Swift 3/4

    S()[keyPath: \.i] = 1
    // expected-error@-1 {{cannot assign through subscript: function call returns immutable value}}
  }
}

func test() {
  let _: WritableKeyPath<C, Int> = \.i // no error for Swift 3/4

  C()[keyPath: \.i] = 1   // warning on write with literal keypath
  // expected-warning@-1 {{forming a writable keypath to property}}

  let _ = C()[keyPath: \.i] // no warning for a read
}

// https://github.com/apple/swift/issues/49887

class Some<T, V> { // expected-note {{'V' declared as parameter to type 'Some'}}
  init(keyPath: KeyPath<T, ((V) -> Void)?>) {
  }
}

class Demo {
  var here: (() -> Void)?
}

let some = Some(keyPath: \Demo.here)
// expected-error@-1 {{cannot convert value of type 'KeyPath<Demo, (() -> Void)?>' to expected argument type 'KeyPath<Demo, ((V) -> Void)?>'}}
// expected-note@-2 {{arguments to generic parameter 'Value' ('(() -> Void)?' and '((V) -> Void)?') are expected to be equal}}
// expected-error@-3 {{generic parameter 'V' could not be inferred}}
// expected-note@-4 {{explicitly specify the generic arguments to fix this issue}}

// SE-0249
func testFunc() {
  let _: (S) -> Int = \.i
  _ = ([S]()).map(\.i)
  _ = \S.init // expected-error {{key path cannot refer to initializer 'init()'}}
  _ = ([S]()).map(\.init) // expected-error {{key path cannot refer to initializer 'init()'}}

  let kp = \S.i
  let _: KeyPath<S, Int> = kp // works, because type defaults to KeyPath nominal
  let f = \S.i
  let _: (S) -> Int = f // expected-error {{cannot convert value of type 'KeyPath<S, Int>' to specified type '(S) -> Int'}}
}

// https://github.com/apple/swift/issues/54871
struct S_54871 {
  static func takesKeyPath(_: KeyPath<S_54871.S, String>) -> String { "" }

  struct S {
    let text: String = takesKeyPath(\.text) // okay
  }
}

// https://github.com/apple/swift/issues/53635
public extension Array {
    func sorted<C: Comparable, K: KeyPath<Element, C>>(by keyPath: K) -> Array<Element> {
        let sortedA = self.sorted(by: { $0[keyPath: keyPath] < $1[keyPath: keyPath] })
        return sortedA
    }

  var i: Int { 0 }
}

func takesVariadicFnWithGenericRet<T>(_ fn: (S...) -> T) {}

// rdar://problem/59445486
func testVariadicKeypathAsFunc() {
  // These are okay, the base type of the KeyPath is inferred to be [S].
  let _: (S...) -> Int = \.i
  let _: (S...) -> Int = \Array.i
  takesVariadicFnWithGenericRet(\.i)
  takesVariadicFnWithGenericRet(\Array.i)

  // These are not okay, the KeyPath should have a base that matches the
  // internal parameter type of the function, i.e (S...).
  let _: (S...) -> Int = \S.i // expected-error {{key path value type 'S' cannot be converted to contextual type 'S...'}}
  takesVariadicFnWithGenericRet(\S.i) // expected-error {{key path value type 'S' cannot be converted to contextual type 'S...'}}
}

// rdar://problem/54322807
struct X<T> {
  init(foo: KeyPath<T, Bool>) { }
  init(foo: KeyPath<T, Bool?>) { }
}

struct Wibble {
  var boolProperty = false
}

struct Bar {
  var optWibble: Wibble? = nil
}

class Foo {
  var optBar: Bar? = nil
}

func testFoo<T: Foo>(_: T) {
  let _: X<T> = .init(foo: \.optBar!.optWibble?.boolProperty)
}

// rdar://problem/56131416

enum Rdar56131416 {
  struct Pass<T> {}
  static func f<T, U>(_ value: T, _ prop: KeyPath<T, U>) -> Pass<U> { fatalError() }

  struct Fail<T> {}
  static func f<T, U>(_ value: T, _ transform: (T) -> U) -> Fail<U> { fatalError() }
  
  static func takesCorrectType(_: Pass<UInt>) {}
}

func rdar56131416() {
  // This call should not be ambiguous.
  let result = Rdar56131416.f(1, \.magnitude) // no-error
  
  // This type should be selected correctly.
  Rdar56131416.takesCorrectType(result)
}

func test_mismatch_with_contextual_optional_result() {
  struct A<T> {
    init<U: Collection>(_ data: T, keyPath: KeyPath<T, U?>) {}
  }

  struct B {
    var arr: [Int] = []
  }

  let _ = A(B(), keyPath: \.arr)
  // expected-error@-1 {{key path value type '[Int]' cannot be converted to contextual type '[Int]?'}}
}

// https://github.com/apple/swift/issues/53581
class C_53581 {}
func f_53581(_ c: C_53581!, _ kp: ReferenceWritableKeyPath<C_53581, String?>,
             _ str: String, _ o_str: String?) {
  c[keyPath: kp] = str // OK
  c![keyPath: kp] = str // OK
  c?[keyPath: kp] = str // OK

  c[keyPath: kp] = o_str // OK
  c![keyPath: kp] = o_str // OK
  c?[keyPath: kp] = o_str // OK
}

class KeyPathBase {}
class KeyPathBaseSubtype: KeyPathBase {}
class AnotherBase {}
class AnotherComposeBase {
  var member: KeyPathBase?
}

func key_path_root_mismatch<T>(_ base: KeyPathBase?, subBase: KeyPathBaseSubtype?, _ abase: AnotherComposeBase,
                               _ kp: KeyPath<KeyPathBase, T>, _ kpa: KeyPath<AnotherBase, T>) {
  let _ : T = base[keyPath: kp] // expected-error {{value of optional type 'KeyPathBase?' must be unwrapped to a value of type 'KeyPathBase'}}
  // expected-note@-1 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{19-19=!}}
  // expected-note@-2 {{use '?' to access key path subscript only for non-'nil' base values}} {{19-19=?}}
  let _ : T = base[keyPath: kpa] // expected-error {{key path with root type 'AnotherBase' cannot be applied to a base of type 'KeyPathBase?'}}

  // Chained root mismatch
  let _ : T = abase.member[keyPath: kp] // expected-error {{value of optional type 'KeyPathBase?' must be unwrapped to a value of type 'KeyPathBase'}}
  // expected-note@-1 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{27-27=!}}
  // expected-note@-2 {{use '?' to access key path subscript only for non-'nil' base values}} {{27-27=?}}
  let _ : T = abase.member[keyPath: kpa] // expected-error {{key path with root type 'AnotherBase' cannot be applied to a base of type 'KeyPathBase?'}}

  let _ : T = subBase[keyPath: kp] // expected-error {{value of optional type 'KeyPathBaseSubtype?' must be unwrapped to a value of type 'KeyPathBaseSubtype'}}
  // expected-note@-1 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{22-22=!}}
  // expected-note@-2 {{use '?' to access key path subscript only for non-'nil' base values}} {{22-22=?}}
  let _ : T = subBase[keyPath: kpa] // expected-error {{key path with root type 'AnotherBase' cannot be applied to a base of type 'KeyPathBaseSubtype?'}}

}

// https://github.com/apple/swift/issues/55884
func f_55884() {
  func f<T>(_ x: KeyPath<String?, T>) -> T { "1"[keyPath: x] }

  _ = f(\.!.count) // OK
  _ = f(\String?.!.count) // OK
  let _: KeyPath<Int?, Int> = \Optional.!
}

// rdar://85458997 - failed to produce a diagnostic about key path root type
func rdar85458997() {
  struct S<R> {
    init(_: KeyPath<R, String>) {
    }

    init(_: KeyPath<R, Int>) {
    }
  }

  struct V {
    var name: String
  }

  _ = S(\.name)
  // expected-error@-1 {{cannot infer key path type from context; consider explicitly specifying a root type}} {{10-10=<#Root#>}}
}

// https://github.com/apple/swift/issues/65965 - failed to produce correct types for key path capability mismatch
func issue_65965() {
  struct S {
	  var s: String
	  let v: String
  }
	
  let refKP: ReferenceWritableKeyPath<S, String>
  refKP = \.s
  // expected-error@-1 {{key path value type 'WritableKeyPath<S, String>' cannot be converted to contextual type 'ReferenceWritableKeyPath<S, String>'}}
	
  let writeKP: WritableKeyPath<S, String>
  writeKP = \.v
  // expected-error@-1 {{key path value type 'KeyPath<S, String>' cannot be converted to contextual type 'WritableKeyPath<S, String>'}}
}
