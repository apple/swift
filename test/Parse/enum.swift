// RUN: %target-typecheck-verify-swift

// FIXME: this test only passes on platforms which have Float80.
// <rdar://problem/19508460> Floating point enum raw values are not portable

// REQUIRES: CPU=i386 || CPU=x86_64

// Windows does not support FP80
// XFAIL: OS=windows-msvc
// XFAIL: OS=freebsd

enum Empty {}

enum Boolish {
  case falsy
  case truthy

  init() { self = .falsy }
}

var b = Boolish.falsy
b = .truthy

enum Optionable<T> {
  case Nought
  case Mere(T)
}

var o = Optionable<Int>.Nought
o = .Mere(0)

enum Color { case Red, Green, Grayscale(Int), Blue }

var c = Color.Red
c = .Green
c = .Grayscale(255)
c = .Blue

let partialApplication = Color.Grayscale

// Cases are excluded from non-enums.
case FloatingCase // expected-error{{enum 'case' is not allowed outside of an enum}}

struct SomeStruct {
  case StructCase // expected-error{{enum 'case' is not allowed outside of an enum}}
}

class SomeClass {
  case ClassCase // expected-error{{enum 'case' is not allowed outside of an enum}}
}

enum EnumWithExtension1 {
  case A1
}
extension EnumWithExtension1 {
  case A2 // expected-error{{enum 'case' is not allowed outside of an enum}}
}

// Attributes for enum cases.

enum EnumCaseAttributes {
  @xyz case EmptyAttributes  // expected-error {{unknown attribute 'xyz'}}
}

// Recover when a switch 'case' label is spelled inside an enum (or outside).
enum SwitchEnvy {
  case X: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case X(Y): // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case X, Y: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case X where true: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case X(Y), Z(W): // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case X(Y) where true: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case 0: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case _: // expected-error{{'case' label can only appear inside a 'switch' statement}}
  case (_, var x, 0): // expected-error{{'case' label can only appear inside a 'switch' statement}}
}

enum HasMethodsPropertiesAndCtors {
  case TweedleDee
  case TweedleDum

  func method() {}
  func staticMethod() {}

  init() {}

  subscript(x:Int) -> Int {
    return 0
  }

  var property : Int {
    return 0
  }
}

enum ImproperlyHasIVars {
  case Flopsy
  case Mopsy

  var ivar : Int // expected-error{{enums must not contain stored properties}}
}

// We used to crash on this.  rdar://14678675
enum rdar14678675 {
  case U1, // expected-error{{expected identifier after comma in enum 'case' declaration}}
  case U2 
  case U3
}

enum Recovery1 {
  case: // expected-error {{'case' label can only appear inside a 'switch' statement}} expected-error {{expected pattern}}
}
enum Recovery2 {
  case UE1: // expected-error {{'case' label can only appear inside a 'switch' statement}}
}
enum Recovery3 {
  case UE2(Void): // expected-error {{'case' label can only appear inside a 'switch' statement}}
}
enum Recovery4 { // expected-note {{in declaration of 'Recovery4'}}
  case Self Self // expected-error {{keyword 'Self' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-12=`Self`}} expected-error {{consecutive declarations on a line must be separated by ';'}} {{12-12=;}} expected-error {{expected declaration}}
}
enum Recovery5 {
  case .UE3 // expected-error {{extraneous '.' in enum 'case' declaration}} {{8-9=}}
  case .UE4, .UE5
  // expected-error@-1{{extraneous '.' in enum 'case' declaration}} {{8-9=}}
  // expected-error@-2{{extraneous '.' in enum 'case' declaration}} {{14-15=}}
}
enum Recovery6 {
  case Snout, _; // expected-error {{keyword '_' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{15-16=`_`}} expected-note {{'_' previously declared here}}
  case _; // expected-error {{keyword '_' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-9=`_`}} expected-error {{invalid redeclaration of '_'}}
  case Tusk, // expected-error {{expected identifier after comma in enum 'case' declaration}}
} 

enum RawTypeEmpty : Int {} // expected-error {{an enum with no cases cannot declare a raw type}}
// expected-error@-1{{'RawTypeEmpty' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}

enum Raw : Int {
  case Ankeny, Burnside
}

enum MultiRawType : Int64, Int32 { // expected-error {{multiple enum raw types 'Int64' and 'Int32'}}
  case Couch, Davis
}

protocol RawTypeNotFirstProtocol {}
enum RawTypeNotFirst : RawTypeNotFirstProtocol, Int { // expected-error {{raw type 'Int' must appear first in the enum inheritance clause}} {{24-24=Int, }} {{47-52=}}
  case E
}

enum ExpressibleByRawTypeNotLiteral : Array<Int> { // expected-error {{raw type 'Array<Int>' is not expressible by a string, integer, or floating-point literal}}
  // expected-error@-1{{'ExpressibleByRawTypeNotLiteral' declares raw type 'Array<Int>', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Ladd, Elliott, Sixteenth, Harrison
}

enum RawTypeCircularityA : RawTypeCircularityB, ExpressibleByIntegerLiteral { // expected-error {{'RawTypeCircularityA' has a raw type that depends on itself}}
  case Morrison, Belmont, Madison, Hawthorne

  init(integerLiteral value: Int) {
    self = .Morrison
  }
}

enum RawTypeCircularityB : RawTypeCircularityA, ExpressibleByIntegerLiteral { // expected-note {{enum 'RawTypeCircularityB' declared here}} 
  case Willamette, Columbia, Sandy, Multnomah

  init(integerLiteral value: Int) { 
    self = .Willamette
  }
}

struct ExpressibleByFloatLiteralOnly : ExpressibleByFloatLiteral {
    init(floatLiteral: Double) {}
}
enum ExpressibleByRawTypeNotIntegerLiteral : ExpressibleByFloatLiteralOnly { // expected-error {{'ExpressibleByRawTypeNotIntegerLiteral' declares raw type 'ExpressibleByFloatLiteralOnly', but does not conform to RawRepresentable and conformance could not be synthesized}} expected-error {{RawRepresentable conformance cannot be synthesized because raw type 'ExpressibleByFloatLiteralOnly' is not Equatable}}
  case Everett // expected-error {{enum cases require explicit raw values when the raw type is not expressible by integer or string literal}}
  case Flanders
}

enum RawTypeWithIntValues : Int {
  case Glisan = 17, Hoyt = 219, Irving, Johnson = 97209
}

enum RawTypeWithNegativeValues : Int {
  case Glisan = -17, Hoyt = -219, Irving, Johnson = -97209
  case AutoIncAcrossZero = -1, Zero, One
}

enum RawTypeWithUnicodeScalarValues : UnicodeScalar { // expected-error {{'RawTypeWithUnicodeScalarValues' declares raw type 'UnicodeScalar' (aka 'Unicode.Scalar'), but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Kearney = "K"
  case Lovejoy // expected-error {{enum cases require explicit raw values when the raw type is not expressible by integer or string literal}}
  case Marshall = "M"
}

enum RawTypeWithCharacterValues : Character { // expected-error {{'RawTypeWithCharacterValues' declares raw type 'Character', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case First = "い"
  case Second // expected-error {{enum cases require explicit raw values when the raw type is not expressible by integer or string literal}}
  case Third = "は"
}

enum RawTypeWithCharacterValues_Correct : Character {
  case First = "😅" // ok
  case Second = "👩‍👩‍👧‍👦" // ok
  case Third = "👋🏽" // ok
  case Fourth = "\u{1F3F4}\u{E0067}\u{E0062}\u{E0065}\u{E006E}\u{E0067}\u{E007F}" // ok
}

enum RawTypeWithCharacterValues_Error1 : Character { // expected-error {{'RawTypeWithCharacterValues_Error1' declares raw type 'Character', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case First = "abc" // expected-error {{cannot convert value of type 'String' to raw type 'Character'}}
}

enum RawTypeWithFloatValues : Float { // expected-error {{'RawTypeWithFloatValues' declares raw type 'Float', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Northrup = 1.5
  case Overton // expected-error {{enum case must declare a raw value when the preceding raw value is not an integer}}
  case Pettygrove = 2.25
}

enum RawTypeWithStringValues : String {
  case Primrose // okay
  case Quimby = "Lucky Lab"
  case Raleigh // okay
  case Savier = "McMenamin's", Thurman = "Kenny and Zuke's"
}

enum RawValuesWithoutRawType {
  case Upshur = 22 // expected-error {{enum case cannot have a raw value if the enum does not have a raw type}}
}

enum RawTypeWithRepeatValues : Int {
  case Vaughn = 22 // expected-note {{raw value previously used here}}
  case Wilson = 22 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValues2 : Double {
  case Vaughn = 22   // expected-note {{raw value previously used here}}
  case Wilson = 22.0 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValues3 : Double {
  // 2^63-1
  case Vaughn = 9223372036854775807   // expected-note {{raw value previously used here}}
  case Wilson = 9223372036854775807.0 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValues4 : Double {
  // 2^64-1
  case Vaughn = 18446744073709551615   // expected-note {{raw value previously used here}}
  case Wilson = 18446744073709551615.0 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValues5 : Double {
  // FIXME: should reject.
  // 2^65-1
  case Vaughn = 36893488147419103231
  case Wilson = 36893488147419103231.0
}

enum RawTypeWithRepeatValues6 : Double {
  // FIXME: should reject.
  // 2^127-1
  case Vaughn = 170141183460469231731687303715884105727
  case Wilson = 170141183460469231731687303715884105727.0
}

enum RawTypeWithRepeatValues7 : Double {
  // FIXME: should reject.
  // 2^128-1
  case Vaughn = 340282366920938463463374607431768211455
  case Wilson = 340282366920938463463374607431768211455.0
}

enum RawTypeWithRepeatValues8 : String {
  case Vaughn = "XYZ" // expected-note {{raw value previously used here}}
  case Wilson = "XYZ" // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithNonRepeatValues : Double {
  case SantaClara = 3.7
  case SanFernando = 7.4
  case SanAntonio = -3.7
  case SanCarlos = -7.4
}


enum RawTypeWithRepeatValuesAutoInc : Double {
  case Vaughn = 22 // expected-note {{raw value auto-incremented from here}}
  case Wilson    // expected-note {{raw value previously used here}}
  case Yeon = 23 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValuesAutoInc2 : Double {
  case Vaughn = 23 // expected-note {{raw value previously used here}}
  case Wilson = 22 // expected-note {{raw value auto-incremented from here}}
  case Yeon // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValuesAutoInc3 : Double {
  case Vaughn // expected-note {{raw value implicitly auto-incremented from zero}}
  case Wilson // expected-note {{raw value previously used here}}
  case Yeon = 1 // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValuesAutoInc4 : String {
  case A = "B" // expected-note {{raw value previously used here}}
  case B // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValuesAutoInc5 : String {
  case A // expected-note {{raw value previously used here}}
  case B = "A" // expected-error {{raw value for enum case is not unique}}
}

enum RawTypeWithRepeatValuesAutoInc6 : String {
  case A
  case B // expected-note {{raw value previously used here}}
  case C = "B" // expected-error {{raw value for enum case is not unique}}
}

enum NonliteralRawValue : Int {
  case Yeon = 100 + 20 + 3 // expected-error {{raw value for enum case must be a literal}}
}

enum RawTypeWithPayload : Int { // expected-error {{'RawTypeWithPayload' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}} expected-note {{declared raw type 'Int' here}} expected-note {{declared raw type 'Int' here}}
  case Powell(Int) // expected-error {{enum with raw type cannot have cases with arguments}}
  case Terwilliger(Int) = 17 // expected-error {{enum with raw type cannot have cases with arguments}}
}

enum RawTypeMismatch : Int { // expected-error {{'RawTypeMismatch' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Barbur = "foo" // expected-error {{}}
}

enum DuplicateMembers1 {
  case Foo // expected-note {{'Foo' previously declared here}}
  case Foo // expected-error {{invalid redeclaration of 'Foo'}}
}

enum DuplicateMembers2 {
  case Foo, Bar // expected-note {{'Foo' previously declared here}} expected-note {{'Bar' previously declared here}}
  case Foo // expected-error {{invalid redeclaration of 'Foo'}}
  case Bar // expected-error {{invalid redeclaration of 'Bar'}}
}

enum DuplicateMembers3 {
  case Foo // expected-note {{'Foo' previously declared here}}
  case Foo(Int) // expected-error {{invalid redeclaration of 'Foo'}}
}

enum DuplicateMembers4 : Int { // expected-error {{'DuplicateMembers4' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Foo = 1 // expected-note {{'Foo' previously declared here}}
  case Foo = 2 // expected-error {{invalid redeclaration of 'Foo'}}
}

enum DuplicateMembers5 : Int { // expected-error {{'DuplicateMembers5' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Foo = 1 // expected-note {{'Foo' previously declared here}}
  case Foo = 1 + 1 // expected-error {{invalid redeclaration of 'Foo'}} expected-error {{raw value for enum case must be a literal}}
}

enum DuplicateMembers6 {
  case Foo // expected-note 2{{'Foo' previously declared here}}
  case Foo // expected-error {{invalid redeclaration of 'Foo'}}
  case Foo // expected-error {{invalid redeclaration of 'Foo'}}
}

enum DuplicateMembers7 : String { // expected-error {{'DuplicateMembers7' declares raw type 'String', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Foo // expected-note {{'Foo' previously declared here}}
  case Foo = "Bar" // expected-error {{invalid redeclaration of 'Foo'}}
}

// Refs to duplicated enum cases shouldn't crash the compiler.
// rdar://problem/20922401
func check20922401() -> String {
  let x: DuplicateMembers1 = .Foo 
  switch x {
    case .Foo:
      return "Foo"
  }
}

enum PlaygroundRepresentation : UInt8 {
  case Class = 1
  case Struct = 2
  case Tuple = 3
  case Enum = 4
  case Aggregate = 5
  case Container = 6
  case IDERepr = 7
  case Gap = 8
  case ScopeEntry = 9
  case ScopeExit = 10
  case Error = 11
  case IndexContainer = 12
  case KeyContainer = 13
  case MembershipContainer = 14
  case Unknown = 0xFF

  static func fromByte(byte : UInt8) -> PlaygroundRepresentation {
    let repr = PlaygroundRepresentation(rawValue: byte)
    if repr == .none { return .Unknown } else { return repr! }
  }
}

struct ManyLiteralable : ExpressibleByIntegerLiteral, ExpressibleByStringLiteral, Equatable {
  init(stringLiteral: String) {}
  init(integerLiteral: Int) {}

  init(unicodeScalarLiteral: String) {}
  init(extendedGraphemeClusterLiteral: String) {}
}
func ==(lhs: ManyLiteralable, rhs: ManyLiteralable) -> Bool { return true }

enum ManyLiteralA : ManyLiteralable {
  case A // expected-note {{raw value previously used here}} expected-note {{raw value implicitly auto-incremented from zero}}
  case B = 0 // expected-error {{raw value for enum case is not unique}}
}

enum ManyLiteralB : ManyLiteralable { // expected-error {{'ManyLiteralB' declares raw type 'ManyLiteralable', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case A = "abc"
  case B // expected-error {{enum case must declare a raw value when the preceding raw value is not an integer}}
}

enum ManyLiteralC : ManyLiteralable {
  case A
  case B = "0"
}

// rdar://problem/22476643
public protocol RawValueA: RawRepresentable
{
  var rawValue: Double { get }
}

enum RawValueATest: Double, RawValueA {
  case A, B
}

public protocol RawValueB
{
  var rawValue: Double { get }
}

enum RawValueBTest: Double, RawValueB {
  case A, B
}

enum foo : String { // expected-error {{'foo' declares raw type 'String', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case bar = nil // expected-error {{cannot convert 'nil' to raw type 'String'}}
}

// Static member lookup from instance methods

struct EmptyStruct {}

enum EnumWithStaticMember {
  static let staticVar = EmptyStruct()

  func foo() {
    let _ = staticVar // expected-error {{static member 'staticVar' cannot be used on instance of type 'EnumWithStaticMember'}}
  }
}

// SE-0036:

struct SE0036_Auxiliary {}

enum SE0036 {
  case A
  case B(SE0036_Auxiliary)
  case C(SE0036_Auxiliary)

  static func staticReference() {
    _ = A
    _ = self.A
    _ = SE0036.A
  }

  func staticReferenceInInstanceMethod() {
    _ = A // expected-error {{enum case 'A' cannot be used as an instance member}} {{9-9=SE0036.}}
    _ = self.A // expected-error {{enum case 'A' cannot be used as an instance member}} {{9-13=SE0036}}
    _ = SE0036.A
  }

  static func staticReferenceInSwitchInStaticMethod() {
    switch SE0036.A {
    case A: break
    case B(_): break
    case C(let x): _ = x; break
    }
  }

  func staticReferenceInSwitchInInstanceMethod() {
    switch self {
    case A: break // expected-error {{enum case 'A' cannot be used as an instance member}} {{10-10=.}}
    case B(_): break // expected-error {{'_' can only appear in a pattern or on the left side of an assignment}}
    case C(let x): _ = x; break // expected-error {{enum case 'C' cannot be used as an instance member}} {{10-10=.}}
    }
  }

  func explicitReferenceInSwitch() {
    switch SE0036.A {
    case SE0036.A: break
    case SE0036.B(_): break
    case SE0036.C(let x): _ = x; break
    }
  }

  func dotReferenceInSwitchInInstanceMethod() {
    switch self {
    case .A: break
    case .B(_): break
    case .C(let x): _ = x; break
    }
  }

  static func dotReferenceInSwitchInStaticMethod() {
    switch SE0036.A {
    case .A: break
    case .B(_): break
    case .C(let x): _ = x; break
    }
  }

  init() {
    self = .A
    self = A // expected-error {{enum case 'A' cannot be used as an instance member}} {{12-12=SE0036.}}
    self = SE0036.A
    self = .B(SE0036_Auxiliary())
    self = B(SE0036_Auxiliary()) // expected-error {{enum case 'B' cannot be used as an instance member}} {{12-12=SE0036.}}
    self = SE0036.B(SE0036_Auxiliary())
  }
}

enum SE0036_Generic<T> {
  case A(x: T)

  func foo() {
    switch self {
    case A(_): break // expected-error {{'_' can only appear in a pattern or on the left side of an assignment}}
    }

    switch self {
    case .A(let a): print(a)
    }

    switch self {
    case SE0036_Generic.A(let a): print(a)
    }
  }
}

enum switch {} // expected-error {{keyword 'switch' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{6-12=`switch`}}

enum SE0155 {
  case emptyArgs() // expected-warning {{enum element with associated values must have at least one associated value}}
  // expected-note@-1 {{did you mean to remove the empty associated value list?}} {{17-19=}}
  // expected-note@-2 {{did you mean to explicitly add a 'Void' associated value?}} {{18-18=Void}}
}

// SR-11261
enum SR11261 {
  case identifier
  case operator // expected-error {{keyword 'operator' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-16=`operator`}}
  case identifier2
}

enum SR11261_var {
  case identifier
  case var // expected-error {{keyword 'var' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-11=`var`}}
  case identifier2
}

enum SR11261_underscore {
  case identifier
  case _ // expected-error {{keyword '_' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-9=`_`}}
  case identifier2
}

enum SR11261_Comma {
  case a, b, c, func, d // expected-error {{keyword 'func' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{17-21=`func`}}
}

enum SR11261_Newline {
  case identifier1
  case identifier2
  case 
  case identifier // expected-error {{keyword 'case' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{3-7=`case`}}
}

enum SR11261_Newline2 {
  case 
  func foo() {} // expected-error {{keyword 'func' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{3-7=`func`}}
}

enum SR11261_PatternMatching {
  case let .foo(x, y): // expected-error {{'case' label can only appear inside a 'switch' statement}}
}

enum CasesWithMissingElement: Int {
  // expected-error@-1 {{'CasesWithMissingElement' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}

  case a = "hello", // expected-error{{expected identifier after comma in enum 'case' declaration}}
  // expected-error@-1 {{cannot convert value of type 'String' to raw type 'Int'}}

  case b = "hello", // expected-error{{expected identifier after comma in enum 'case' declaration}}
  // expected-error@-1 {{cannot convert value of type 'String' to raw type 'Int'}}
}
