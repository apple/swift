// RUN: %target-typecheck-verify-swift -parse-as-library

// Protocols can be nested inside non-generic types.

protocol Delegate {}

enum Table {
  protocol Delegate {}
}

enum Button {
  protocol Delegate {}
}

struct PlainDelegateConformer: Delegate {}
struct TableDelegateConformer: Table.Delegate {}
struct ButtonDelegateConformer: Button.Delegate {}

func testDifferent() {
  let plain = PlainDelegateConformer()
  if plain is Delegate { print("ok (1)") } // expected-warning {{'is' test is always true}}
  if plain is Table.Delegate { print("bad (2)") }
  if plain is Button.Delegate { print("bad (3)") }

  let tableDel = TableDelegateConformer()
  if tableDel is Delegate { print("bad (4)") }
  if tableDel is Table.Delegate { print("ok (5)") } // expected-warning {{'is' test is always true}}
  if tableDel is Button.Delegate { print("bad (6)") }

  let buttonDel = ButtonDelegateConformer()
  if buttonDel is Delegate { print("bad (7)") }
  if buttonDel is Table.Delegate { print("bad (8)") }
  if buttonDel is Button.Delegate { print("ok (9)") } // expected-warning {{'is' test is always true}}
}

enum OuterEnum {
  protocol C {}
  case C(C) 
  // expected-error@-1{{invalid redeclaration of 'C'}}
  // expected-note@-3{{'C' previously declared here}}
}

class OuterClass {
  protocol InnerProtocol : OuterClass { }
}

// Name lookup circularity tests.

protocol SomeBaseProtocol {}

struct ConformanceOnDecl: ConformanceOnDecl.P {
    protocol P: SomeBaseProtocol {}
}
struct ConformanceOnDecl_2: ConformanceOnDecl_2.P {
    protocol P where Self: SomeBaseProtocol {}
}
struct ConformanceOnDecl_3: Self.P {
    protocol P: SomeBaseProtocol {}
}
struct ConformanceOnDecl_4: ConformanceOnDecl_4.Q {
    protocol P: SomeBaseProtocol {}
    protocol Q: P {}
}


struct ConformanceInExtension {
    protocol P: SomeBaseProtocol {}
}
extension ConformanceInExtension: ConformanceInExtension.P {}

struct ConformanceInExtension_2 {
    protocol P where Self: SomeBaseProtocol {}
}
extension ConformanceInExtension_2: ConformanceInExtension_2.P {}

struct ConformanceInExtension_3 {
    protocol P: SomeBaseProtocol {}
}
extension ConformanceInExtension_3: Self.P {}

struct ConformanceInExtension_4 {
    protocol P: SomeBaseProtocol {}
    protocol Q: P {}
}
extension ConformanceInExtension_4: ConformanceInExtension_4.Q {}

// Protocols can be nested in actors.

@available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *)
actor SomeActor {
  protocol Proto {}
}

// Deeply nested.

enum Level0 {
  struct Level1 {
    class Level2 {
      enum Level3 {
        struct Level4 {
          class Level5 {
            protocol DeeplyNested {
              func someRequirement()
            }
            func lookupTest(_ input: some DeeplyNested) {}
          }
          func lookupTest(_ input: some Level5.DeeplyNested) {}
        }
        func lookupTest(_ input: some Level4.Level5.DeeplyNested) {}
      }
    }
  }
}

func useDeeplyNested(_ input: some Level0.Level1.Level2.Level3.Level4.Level5.DeeplyNested) {
  input.someRequirement()
}

// Add a nested protocol in an extension.

extension Level0.Level1.Level2 {
  protocol ModeratelyNested {
    associatedtype ReturnValue
    func anotherRequirement() -> ReturnValue
  }

  func lookupTest(_: some ModeratelyNested) { }
}

func useModeratelyNested<Input>(
  _ input: Input
) -> Input.ReturnValue where Input: Level0.Level1.Level2.ModeratelyNested {
  input.anotherRequirement()
}

struct ModeratelyNestedConformer: Level0.Level1.Level2.ModeratelyNested {
  func anotherRequirement() -> Int {
    99
  }
}

let _umn: Int = useModeratelyNested(ModeratelyNestedConformer())

// Extend a nested protocol.

extension Level0.Level1.Level2.ModeratelyNested {
  func extensionMethod() -> ReturnValue {
    anotherRequirement()
  }
}

extension Level0.Level1.Level2.ModeratelyNested where ReturnValue == Int {
  func extensionMethod() -> Int {
    anotherRequirement() + 1
  }
}

// Protocols may be nested inside non-generic functions.

func someFunction_0() {
  protocol Yes {}
  struct Foo: Yes {}
  struct Bar: Yes {}
}

func someFunction_1() {

  struct HowAboutThis {
    protocol AlsoYes {
      associatedtype SomeType
      func doSomething() -> SomeType
    }
  }

  struct Conforms0: HowAboutThis.AlsoYes {
    typealias SomeType = Int
    func doSomething() -> SomeType { -99 }
  }

  struct Conforms1: HowAboutThis.AlsoYes {
    typealias SomeType = String
    func doSomething() -> SomeType { "works" }
  }

  func usesNested<T: HowAboutThis.AlsoYes>(_ input: T) -> T.SomeType {
    input.doSomething()
  }

  let _: Int = usesNested(Conforms0())
  let _: String = usesNested(Conforms1())
}

// Unqualified lookup searches parent scopes.

struct Lookup_Parent {
  protocol A {}
  class SomeClass {}
}

extension Lookup_Parent {
  protocol B: A {}
  protocol C: SomeClass, B {
    func doSomething(_: some A) -> SomeClass
  }
}

// Protocols cannot be nested in generic types or other protocols.

struct OuterGeneric<D> {
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested in a generic context}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ t: D) // expected-error {{cannot find type 'D' in scope}}
  }
}

class OuterGenericClass<T> {
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested in a generic context}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ t: T) // expected-error {{cannot find type 'T' in scope}}
  }
}

protocol OuterProtocol {
  associatedtype Hen
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested in protocol 'OuterProtocol'}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ h: Hen) // expected-error {{cannot find type 'Hen' in scope}}
  }
}

extension OuterProtocol {
  protocol DefinedInExtension {} // expected-error{{protocol 'DefinedInExtension' cannot be nested in protocol 'OuterProtocol'}}
}

struct ConformsToOuterProtocol : OuterProtocol {
  typealias Hen = Int
  func f() { let _ = InnerProtocol.self } // expected-error {{use of protocol 'InnerProtocol' as a type must be written 'any InnerProtocol'}}
}

extension OuterProtocol {
  func f() {
    protocol Invalid_0 {} // expected-error{{type 'Invalid_0' cannot be nested in generic function 'f()'}}

    struct SomeType { // expected-error{{type 'SomeType' cannot be nested in generic function 'f()'}}
      protocol Invalid_1 {} // expected-error{{protocol 'Invalid_1' cannot be nested in a generic context}}
    }
  }
  func g<T>(_: T) {
    protocol Invalid_2 {} // expected-error{{type 'Invalid_2' cannot be nested in generic function 'g'}}
  }
}

// 'InnerProtocol' does not inherit the generic parameters of
// 'OtherGenericClass', so the occurrence of 'OtherGenericClass'
// in 'InnerProtocol' is not "in context" with implicitly
// inferred generic arguments <T, U>.
class OtherGenericClass<T, U> { // expected-note {{generic type 'OtherGenericClass' declared here}}
  protocol InnerProtocol : OtherGenericClass { }
  // expected-error@-1 {{protocol 'InnerProtocol' cannot be nested in a generic context}}
  // expected-error@-2 {{reference to generic type 'OtherGenericClass' requires arguments in <...>}}
}

protocol InferredGenericTest {
  associatedtype A
}
extension OtherGenericClass {
  protocol P: InferredGenericTest where Self.A == T {}
  // expected-error@-1 {{protocol 'P' cannot be nested in a generic context}}
  // expected-error@-2 {{cannot find type 'T' in scope}}
}

// A nested protocol does not satisfy an associated type requirement.

protocol HasAssoc {
  associatedtype A // expected-note {{protocol requires nested type 'A'; add nested type 'A' for conformance}}
}
struct ConformsToHasAssoc: HasAssoc { // expected-error {{type 'ConformsToHasAssoc' does not conform to protocol 'HasAssoc'}}
  protocol A {}
}

// @usableFromInline.

struct OuterForUFI {
  @usableFromInline
  protocol Inner {
    func req()
  }
}

extension OuterForUFI.Inner {
  public func extMethod() {} // The 'public' puts this in a special path.
}

func testLookup(_ x: OuterForUFI.Inner) {
  x.req()
  x.extMethod()
}

func testLookup<T: OuterForUFI.Inner>(_ x: T) {
  x.req()
  x.extMethod()
}

// Protocols cannot be nested inside of generic functions.

func invalidProtocolInGeneric<T>(_: T) {
  protocol Test {} // expected-error{{type 'Test' cannot be nested in generic function 'invalidProtocolInGeneric'}}
}

struct NestedInGenericMethod<T> {
  func someMethod() {
    protocol AnotherTest {} // expected-error{{type 'AnotherTest' cannot be nested in generic function 'someMethod()'}}
  }
}

// Types cannot be nested inside of protocols.

protocol Racoon {
  associatedtype Stripes
  class Claw<T> { // expected-error{{type 'Claw' cannot be nested in protocol 'Racoon'}}
    func mangle(_ s: Stripes) {}
    // expected-error@-1 {{cannot find type 'Stripes' in scope}}
  }
  struct Fang<T> { // expected-error{{type 'Fang' cannot be nested in protocol 'Racoon'}}
    func gnaw(_ s: Stripes) {}
    // expected-error@-1 {{cannot find type 'Stripes' in scope}}
  }
  enum Fur { // expected-error{{type 'Fur' cannot be nested in protocol 'Racoon'}}
    case Stripes
  }
}

enum SillyRawEnum : SillyProtocol.InnerClass {} // expected-error {{an enum with no cases cannot declare a raw type}}
// expected-error@-1 {{reference to generic type 'SillyProtocol.InnerClass' requires arguments in <...>}}

protocol SillyProtocol {
  class InnerClass<T> {} // expected-error {{type 'InnerClass' cannot be nested in protocol 'SillyProtocol'}}
  // expected-note@-1 {{generic type 'InnerClass' declared here}}
}

protocol SelfDotTest {
  func f(_: Self.Class)
  class Class {}
  // expected-error@-1{{type 'Class' cannot be nested in protocol 'SelfDotTest'}}
}

struct Outer {
  typealias E = NestedValidation.T
  protocol NestedValidation {
    typealias T = A.B
    class A { // expected-error {{type 'A' cannot be nested in protocol 'NestedValidation'}}
      typealias B = Int
    }
  }
}

// https://github.com/apple/swift/issues/68284
protocol P {
  associatedtype A
}
func generic(value: P) {}   // expected-error {{use of protocol 'P' as a type must be written 'any P' or 'some P'}}
                            // expected-note@-1 {{replace 'P' with 'any P'}} {{21-22=any P}}
                            // expected-note@-2 {{replace 'P' with 'some P'}} {{21-22=some P}}
