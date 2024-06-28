// RUN: %target-typecheck-verify-swift -enable-objc-interop

protocol EmptyProtocol { }

protocol DefinitionsInProtocols {
  init() {} // expected-error {{protocol initializers must not have bodies}}
  deinit {} // expected-error {{deinitializers may only be declared within a class, actor, or noncopyable type}}
}

// Protocol decl.
protocol Test {
  func setTitle(_: String)
  func erase() -> Bool

  var creator: String { get }
  var major : Int { get }
  var minor : Int { get }
  var subminor : Int  // expected-error {{property in protocol must have explicit { get } or { get set } specifier}} {{21-21= { get <#set#> \}}}
  static var staticProperty: Int // expected-error{{property in protocol must have explicit { get } or { get set } specifier}} {{33-33= { get <#set#> \}}}

  let bugfix // expected-error {{type annotation missing in pattern}} expected-error {{protocols cannot require properties to be immutable; declare read-only properties by using 'var' with a '{ get }' specifier}}
  var comment // expected-error {{type annotation missing in pattern}} expected-error {{property in protocol must have explicit { get } or { get set } specifier}}
}

protocol Test2 {
  var property: Int { get }

  var title: String = "The Art of War" { get } // expected-error{{initial value is not allowed here}} expected-error {{property in protocol must have explicit { get } or { get set } specifier}} {{20-20= { get <#set#> \}}}
  static var title2: String = "The Art of War" // expected-error{{initial value is not allowed here}} expected-error {{property in protocol must have explicit { get } or { get set } specifier}} {{28-28= { get <#set#> \}}}

  associatedtype mytype
  associatedtype mybadtype = Int

  associatedtype V : Test = // expected-error {{expected type in associated type declaration}} {{28-28= <#type#>}}
}

func test1() {
  var v1: Test
  var s: String 
  
  v1.setTitle(s)
  v1.creator = "Me"                   // expected-error {{cannot assign to property: 'creator' is a get-only property}}
}

protocol Bogus : Int {}
// expected-error@-1{{inheritance from non-protocol, non-class type 'Int'}}
// expected-error@-2{{type 'Self' constrained to non-protocol, non-class type 'Int'}}

// Explicit conformance checks (successful).

protocol CustomStringConvertible { func print() } // expected-note{{protocol requires function 'print()' with type '() -> ()'}} expected-note{{protocol requires}} expected-note{{protocol requires}} expected-note{{protocol requires}}

struct TestFormat { }
protocol FormattedPrintable : CustomStringConvertible {
  func print(format: TestFormat)
}

struct X0 : Any, CustomStringConvertible {
  func print() {}
}

class X1 : Any, CustomStringConvertible {
  func print() {}
}

enum X2 : Any { }

extension X2 : CustomStringConvertible {
  func print() {}
}

// Explicit conformance checks (unsuccessful)

struct NotPrintableS : Any, CustomStringConvertible {} // expected-error{{type 'NotPrintableS' does not conform to protocol 'CustomStringConvertible'}}

class NotPrintableC : CustomStringConvertible, Any {} // expected-error{{type 'NotPrintableC' does not conform to protocol 'CustomStringConvertible'}}

enum NotPrintableO : Any, CustomStringConvertible {} // expected-error{{type 'NotPrintableO' does not conform to protocol 'CustomStringConvertible'}}

struct NotFormattedPrintable : FormattedPrintable { // expected-error{{type 'NotFormattedPrintable' does not conform to protocol 'CustomStringConvertible'}}
  func print(format: TestFormat) {} 
}

// Protocol compositions in inheritance clauses
protocol Left {
  func l() // expected-note {{protocol requires function 'l()' with type '() -> ()'; add a stub for conformance}}
}
protocol Right {
  func r() // expected-note {{protocol requires function 'r()' with type '() -> ()'; add a stub for conformance}}
}
typealias Both = Left & Right

protocol Up : Both {
  func u()
}

struct DoesNotConform : Up {
  // expected-error@-1 {{type 'DoesNotConform' does not conform to protocol 'Left'}}
  // expected-error@-2 {{type 'DoesNotConform' does not conform to protocol 'Right'}}
  func u() {}
}

// Circular protocols

protocol CircleMiddle : CircleStart { func circle_middle() }
// expected-note@-1 3 {{protocol 'CircleMiddle' declared here}}
protocol CircleStart : CircleEnd { func circle_start() } // expected-error 3 {{protocol 'CircleStart' refines itself}}
protocol CircleEnd : CircleMiddle { func circle_end()} // expected-note 3 {{protocol 'CircleEnd' declared here}}

protocol CircleEntry : CircleTrivial { }
protocol CircleTrivial : CircleTrivial { } // expected-error {{protocol 'CircleTrivial' refines itself}}

struct Circle {
  func circle_start() {}
  func circle_middle() {}
  func circle_end() {}
}

func testCircular(_ circle: Circle) {
  // FIXME: It would be nice if this failure were suppressed because the protocols
  // have circular definitions.
  _ = circle as any CircleStart // expected-error{{cannot convert value of type 'Circle' to type 'any CircleStart' in coercion}}
}

// <rdar://problem/14750346>
protocol Q : C, H { }
protocol C : E { }
protocol H : E { }
protocol E { }

//===----------------------------------------------------------------------===//
// Associated types
//===----------------------------------------------------------------------===//

protocol SimpleAssoc {
  associatedtype Associated // expected-note{{protocol requires nested type 'Associated'}}
}

struct IsSimpleAssoc : SimpleAssoc {
  struct Associated {}
}

struct IsNotSimpleAssoc : SimpleAssoc {} // expected-error{{type 'IsNotSimpleAssoc' does not conform to protocol 'SimpleAssoc'}}

protocol StreamWithAssoc {
  associatedtype Element
  func get() -> Element // expected-note{{protocol requires function 'get()' with type '() -> NotAStreamType.Element'}}
}

struct AnRange<Int> : StreamWithAssoc {
  typealias Element = Int
  func get() -> Int {}
}

// Okay: Word is a typealias for Int
struct AWordStreamType : StreamWithAssoc {
  typealias Element = Int
  func get() -> Int {}
}

struct NotAStreamType : StreamWithAssoc { // expected-error{{type 'NotAStreamType' does not conform to protocol 'StreamWithAssoc'}}
  typealias Element = Float
  func get() -> Int {} // expected-note{{candidate has non-matching type '() -> Int'}}
}

// Okay: Infers Element == Int
struct StreamTypeWithInferredAssociatedTypes : StreamWithAssoc {
  func get() -> Int {}
}

protocol SequenceViaStream {
  associatedtype SequenceStreamTypeType : IteratorProtocol // expected-note{{protocol requires nested type 'SequenceStreamTypeType'}}
  func makeIterator() -> SequenceStreamTypeType
}

struct IntIterator : IteratorProtocol /*, Sequence, ReplPrintable*/ {
  typealias Element = Int
  var min : Int
  var max : Int
  var stride : Int

  mutating func next() -> Int? {
    if min >= max { return .none }
    let prev = min
    min += stride
    return prev
  }

  typealias Generator = IntIterator
  func makeIterator() -> IntIterator {
    return self
  }
}

extension IntIterator : SequenceViaStream {
  typealias SequenceStreamTypeType = IntIterator
}

struct NotSequence : SequenceViaStream { // expected-error{{type 'NotSequence' does not conform to protocol 'SequenceViaStream'}}
  typealias SequenceStreamTypeType = Int // expected-note{{possibly intended match 'NotSequence.SequenceStreamTypeType' (aka 'Int') does not conform to 'IteratorProtocol'}}
  func makeIterator() -> Int {}
}

protocol GetATuple {
  associatedtype Tuple
  func getATuple() -> Tuple
}

struct IntStringGetter : GetATuple {
  typealias Tuple = (i: Int, s: String)
  func getATuple() -> Tuple {}
}

protocol ClassConstrainedAssocType {
  associatedtype T : class
  // expected-error@-1 {{'class' constraint can only appear on protocol declarations}}
  // expected-note@-2 {{did you mean to write an 'AnyObject' constraint?}}{{22-27=AnyObject}}
}

//===----------------------------------------------------------------------===//
// Default arguments
//===----------------------------------------------------------------------===//
// FIXME: Actually make use of default arguments, check substitutions, etc.
protocol ProtoWithDefaultArg {
  func increment(_ value: Int = 1) // expected-error{{default argument not permitted in a protocol method}}
}

struct HasNoDefaultArg : ProtoWithDefaultArg {
  func increment(_: Int) {}
}

//===----------------------------------------------------------------------===//
// Variadic function requirements
//===----------------------------------------------------------------------===//
protocol IntMaxable {
  func intmax(first: Int, rest: Int...) -> Int // expected-note 2{{protocol requires function 'intmax(first:rest:)' with type '(Int, Int...) -> Int'}}
}

struct HasIntMax : IntMaxable {
  func intmax(first: Int, rest: Int...) -> Int {}
}

struct NotIntMax1 : IntMaxable  { // expected-error{{type 'NotIntMax1' does not conform to protocol 'IntMaxable'}}
  func intmax(first: Int, rest: [Int]) -> Int {} // expected-note{{candidate has non-matching type '(Int, [Int]) -> Int'}}
}

struct NotIntMax2 : IntMaxable { // expected-error{{type 'NotIntMax2' does not conform to protocol 'IntMaxable'}}
  func intmax(first: Int, rest: Int) -> Int {} // expected-note{{candidate has non-matching type '(Int, Int) -> Int'}}
}

//===----------------------------------------------------------------------===//
// 'Self' type
//===----------------------------------------------------------------------===//
protocol IsEqualComparable {
  func isEqual(other: Self) -> Bool // expected-note{{protocol requires function 'isEqual(other:)' with type '(WrongIsEqual) -> Bool'}}
}

struct HasIsEqual : IsEqualComparable {
  func isEqual(other: HasIsEqual) -> Bool {}
}

struct WrongIsEqual : IsEqualComparable { // expected-error{{type 'WrongIsEqual' does not conform to protocol 'IsEqualComparable'}}
  func isEqual(other: Int) -> Bool {}  // expected-note{{candidate has non-matching type '(Int) -> Bool'}}
}

//===----------------------------------------------------------------------===//
// Static methods
//===----------------------------------------------------------------------===//
protocol StaticP {
  static func f()
}
protocol InstanceP {
  func f() // expected-note{{protocol requires function 'f()' with type '() -> ()'}}
}
struct StaticS1 : StaticP {
  static func f() {}
}
struct StaticS2 : InstanceP { // expected-error{{type 'StaticS2' does not conform to protocol 'InstanceP'}}
  static func f() {} // expected-note{{candidate operates on a type, not an instance as required}}
}
struct StaticAndInstanceS : InstanceP {
  static func f() {}
  func f() {}
}
func StaticProtocolFunc() {
  let a: StaticP = StaticS1()
  a.f() // expected-error{{static member 'f' cannot be used on instance of type 'any StaticP'}}
}
func StaticProtocolGenericFunc<t : StaticP>(_: t) {
  t.f()
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//
protocol Eq {
  static func ==(lhs: Self, rhs: Self) -> Bool
}

extension Int : Eq { }

// Matching prefix/postfix.
prefix operator <>
postfix operator <>

protocol IndexValue {
  static prefix func <> (_ max: Self) -> Int
  static postfix func <> (min: Self) -> Int
}

prefix func <> (max: Int) -> Int  { return 0 }
postfix func <> (min: Int) -> Int  { return 0 }

extension Int : IndexValue {}

//===----------------------------------------------------------------------===//
// Class protocols
//===----------------------------------------------------------------------===//

protocol IntrusiveListNode : class {
  var next : Self { get }
}

final class ClassNode : IntrusiveListNode {
  var next : ClassNode = ClassNode()
}

struct StructNode : IntrusiveListNode { // expected-error{{non-class type 'StructNode' cannot conform to class protocol 'IntrusiveListNode'}}
  var next : StructNode  // expected-error {{value type 'StructNode' cannot have a stored property that recursively contains it}}

}

final class ClassNodeByExtension { }
struct StructNodeByExtension { }

extension ClassNodeByExtension : IntrusiveListNode {
  var next : ClassNodeByExtension {
    get {
      return self
    }
    set {}
  }
}

extension StructNodeByExtension : IntrusiveListNode { // expected-error{{non-class type 'StructNodeByExtension' cannot conform to class protocol 'IntrusiveListNode'}}
  var next : StructNodeByExtension {
    get {
      return self
    }
    set {}
  }
}

final class GenericClassNode<T> : IntrusiveListNode {
  var next : GenericClassNode<T> = GenericClassNode()
}

struct GenericStructNode<T> : IntrusiveListNode { // expected-error{{non-class type 'GenericStructNode<T>' cannot conform to class protocol 'IntrusiveListNode'}}
  var next : GenericStructNode<T> // expected-error {{value type 'GenericStructNode<T>' cannot have a stored property that recursively contains it}}
}

// Refined protocols inherit class-ness
protocol IntrusiveDListNode : IntrusiveListNode {
  var prev : Self { get }
}

final class ClassDNode : IntrusiveDListNode {
  var prev : ClassDNode = ClassDNode()
  var next : ClassDNode = ClassDNode()
}

struct StructDNode : IntrusiveDListNode { // expected-error{{non-class type 'StructDNode' cannot conform to class protocol 'IntrusiveDListNode'}}
  // expected-error@-1{{non-class type 'StructDNode' cannot conform to class protocol 'IntrusiveListNode'}}
  var prev : StructDNode // expected-error {{value type 'StructDNode' cannot have a stored property that recursively contains it}}
  var next : StructDNode
}

@objc protocol ObjCProtocol {
  func foo() // expected-note{{protocol requires function 'foo()' with type '() -> ()'}}
}
protocol NonObjCProtocol : class { //expected-note{{protocol 'NonObjCProtocol' declared here}}
  func bar()
}

class DoesntConformToObjCProtocol : ObjCProtocol { // expected-error{{type 'DoesntConformToObjCProtocol' does not conform to protocol 'ObjCProtocol'}}
}

@objc protocol ObjCProtocolRefinement : ObjCProtocol { }

@objc protocol ObjCNonObjCProtocolRefinement : NonObjCProtocol { } //expected-error{{@objc protocol 'ObjCNonObjCProtocolRefinement' cannot refine non-@objc protocol 'NonObjCProtocol'}}


// <rdar://problem/16079878>
protocol P1 {
  associatedtype Assoc // expected-note 2{{protocol requires nested type 'Assoc'}}
}

protocol P2 {
}

struct X3<T : P1> where T.Assoc : P2 {}

struct X4 : P1 { // expected-error{{type 'X4' does not conform to protocol 'P1'}}
  func getX1() -> X3<X4> { return X3() }
}

protocol ShouldntCrash {
  // rdar://16109996
  let fullName: String { get }  // expected-error {{'let' declarations cannot be computed properties}} {{3-6=var}}
  
  // <rdar://problem/17200672> Let in protocol causes unclear errors and crashes
  let fullName2: String  // expected-error {{protocols cannot require properties to be immutable; declare read-only properties by using 'var' with a '{ get }' specifier}} {{3-6=var}} {{24-24= { get \}}}

  // <rdar://problem/16789886> Assert on protocol property requirement without a type
  var propertyWithoutType { get } // expected-error {{type annotation missing in pattern}}
  // expected-error@-1 {{computed property must have an explicit type}} {{26-26=: <# Type #>}}
}

// rdar://problem/18168866
protocol FirstProtocol {
    // expected-warning@+1 {{'weak' cannot be applied to a property declaration in a protocol; this is an error in the Swift 5 language mode}}
    weak var delegate : SecondProtocol? { get } // expected-error{{'weak' must not be applied to non-class-bound 'any SecondProtocol'; consider adding a protocol conformance that has a class bound}}
}

protocol SecondProtocol {
    func aMethod(_ object : FirstProtocol)
}

// <rdar://problem/19495341> Can't upcast to parent types of type constraints without forcing
class C1 : P2 {}
func f<T : C1>(_ x : T) {
  _ = x as P2
}

class C2 {}
func g<T : C2>(_ x : T) {
  x as P2 // expected-error{{cannot convert value of type 'T' to type 'any P2' in coercion}}
}

class C3 : P1 {} // expected-error{{type 'C3' does not conform to protocol 'P1'}}
func h<T : C3>(_ x : T) {
  _ = x as any P1
}
func i<T : C3>(_ x : T?) -> Bool {
  return x is any P1
  // expected-warning@-1 {{checking a value with optional type 'T?' against type 'any P1' succeeds whenever the value is non-nil; did you mean to use '!= nil'?}}
}
func j(_ x : C1) -> Bool {
  return x is P1 // expected-error {{use of protocol 'P1' as a type must be written 'any P1'}}
}
func k(_ x : C1?) -> Bool {
  return x is any P1
}


protocol P4 {
  associatedtype T // expected-note {{protocol requires nested type 'T'}}
}

class C4 : P4 { // expected-error {{type 'C4' does not conform to protocol 'P4'}}
  associatedtype T = Int  // expected-error {{associated types can only be defined in a protocol; define a type or introduce a 'typealias' to satisfy an associated type requirement}} {{3-17=typealias}}
}

// <rdar://problem/25185722> Crash with invalid 'let' property in protocol
protocol LetThereBeCrash {
  let x: Int
  // expected-error@-1 {{protocols cannot require properties to be immutable; declare read-only properties by using 'var' with a '{ get }' specifier}} {{13-13= { get \}}}
  // expected-note@-2 {{declared here}}
  let xs: [Int]
  // expected-error@-1 {{protocols cannot require properties to be immutable; declare read-only properties by using 'var' with a '{ get }' specifier}} {{3-6=var}} {{16-16= { get \}}}
  // expected-note@-2 {{declared here}}
}

extension LetThereBeCrash {
  init() { x = 1; xs = [] }
  // expected-error@-1 {{'let' property 'x' may not be initialized directly; use "self.init(...)" or "self = ..." instead}}
  // expected-error@-2 {{'let' property 'xs' may not be initialized directly; use "self.init(...)" or "self = ..." instead}}
}

// https://github.com/apple/swift/issues/53813
// Offer fix-it to conform type of context to the missing protocols

protocol P1_53813 {}
protocol P2_53813 {}
protocol P3_53813 {}
protocol P4_53813: AnyObject {}

class C0_53813 {
  var foo1: P1_53813?
  var foo2: (P1_53813 & P2_53813)?
  weak var foo3: P4_53813?
}

// Context has no inherited types and does not conform to protocol //

class C1_53813 {
  let c0 = C0_53813()

  func conform() {
    c0.foo1 = self // expected-error {{cannot assign value of type 'C1_53813' to type '(any P1_53813)?'}}
    // expected-note@-1 {{add missing conformance to 'P1_53813' to class 'C1_53813'}}{{-4:15-15=: P1_53813}}
  }
}

// Context has no inherited types and does not conform to protocol composition //

class C2_53813 {
  let c0 = C0_53813()

  func conform() {
    c0.foo2 = self // expected-error {{cannot assign value of type 'C2_53813' to type '(any P1_53813 & P2_53813)?'}}
    // expected-note@-1 {{add missing conformance to 'P1_53813 & P2_53813' to class 'C2_53813'}}{{-4:15-15=: P1_53813 & P2_53813}}
  }
}

// Context already has an inherited type, but does not conform to protocol //

class C3_53813: P3_53813 {
  let c0 = C0_53813()

  func conform() {
    c0.foo1 = self // expected-error {{cannot assign value of type 'C3_53813' to type '(any P1_53813)?'}}
    // expected-note@-1 {{add missing conformance to 'P1_53813' to class 'C3_53813'}}{{-4:25-25=, P1_53813}}
  }
}

// Context conforms to only one protocol in the protocol composition //

class C4_53813: P1_53813 {
  let c0 = C0_53813()

  func conform() {
    c0.foo2 = self // expected-error {{cannot assign value of type 'C4_53813' to type '(any P1_53813 & P2_53813)?'}}
    // expected-note@-1 {{add missing conformance to 'P1_53813 & P2_53813' to class 'C4_53813'}}{{-4:25-25=, P2_53813}}
  }
}

// Context is a value type, but protocol requires class //

struct S0_53813 {
  let c0 = C0_53813()

  func conform() {
    c0.foo3 = self // expected-error {{cannot assign value of type 'S0_53813' to type '(any P4_53813)?'}}
  }
}
