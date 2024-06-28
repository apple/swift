// RUN: %target-typecheck-verify-swift \
// RUN: -enable-experimental-feature NonescapableTypes \
// RUN: -enable-experimental-feature SuppressedAssociatedTypes

// expected-note@+1 {{'T' has '~Copyable' constraint preventing implicit 'Copyable' conformance}}
struct AttemptImplicitConditionalConformance<T: ~Copyable>: ~Copyable {
  var t: T // expected-error {{stored property 't' of 'Copyable'-conforming generic struct 'AttemptImplicitConditionalConformance' has non-Copyable type 'T'}}
}
extension AttemptImplicitConditionalConformance: Copyable {}
// expected-error@-1 {{generic struct 'AttemptImplicitConditionalConformance' required to be 'Copyable' but is marked with '~Copyable'}}

enum Hello<T: ~Escapable & ~Copyable>: ~Escapable & ~Copyable {}
extension Hello: Escapable {} // expected-error {{generic enum 'Hello' required to be 'Escapable' but is marked with '~Escapable'}}
extension Hello: Copyable {} // expected-error {{generic enum 'Hello' required to be 'Copyable' but is marked with '~Copyable'}}

enum HelloExplicitlyFixed<T: ~Escapable & ~Copyable>: Escapable, Copyable {}

struct NoInverseBecauseNoDefault<T: ~Copyable & ~Escapable>: ~Copyable {}
extension NoInverseBecauseNoDefault: Copyable where T: Copyable, T: ~Escapable {}
// expected-error@-1 {{cannot suppress '~Escapable' on generic parameter 'T' defined in outer scope}}

// Check support for explicit conditional conformance
public struct ExplicitCond<T: ~Copyable>: ~Copyable {}
extension ExplicitCond: Copyable where T: Copyable {}
// expected-note@-1 {{requirement from conditional conformance}}
// expected-note@-2 {{requirement from conditional conformance of 'ExplicitCondAlias<NC>' (aka 'ExplicitCond<NC>') to 'Copyable'}}

public typealias ExplicitCondAlias<T> = ExplicitCond<T> where T: ~Copyable
public typealias AlwaysCopyable<T> = ExplicitCond<T>

func checkCopyable<T>(_ t: T) {} // expected-note {{'where T: Copyable' is implicit here}}

func test<C, NC: ~Copyable>(
  _ a1: ExplicitCond<C>, _ b1: borrowing ExplicitCond<NC>,
  _ a2: ExplicitCondAlias<C>, _ b2: borrowing ExplicitCondAlias<NC>
  ) {
  checkCopyable(a1)
  checkCopyable(b1) // expected-error {{global function 'checkCopyable' requires that 'NC' conform to 'Copyable'}}
  checkCopyable(a2)
  checkCopyable(b2) // expected-error {{global function 'checkCopyable' requires that 'NC' conform to 'Copyable'}}
}

func checkAliases<C, NC: ~Copyable>(_ a: AlwaysCopyable<C>, _ b: AlwaysCopyable<NC>) {
// expected-error@-1 {{'NC' required to be 'Copyable' but is marked with '~Copyable'}}
  checkCopyable(a)
  checkCopyable(b)
}

protocol NeedsCopyable {}
// expected-note@-1 {{type 'TryInferCopyable' does not conform to inherited protocol 'Copyable'}}

struct TryInferCopyable: ~Copyable, NeedsCopyable {}
// expected-error@-1 {{type 'TryInferCopyable' does not conform to protocol 'Copyable'}}

protocol Removed: ~Copyable {
  func requiresCopyableSelf(_ t: AlwaysCopyable<Self>)
  // expected-error@-1 {{type 'Self' does not conform to protocol 'Copyable'}}
}
protocol Plain<T> {
  associatedtype T: ~Copyable
  func requiresCopyableSelf(_ t: AlwaysCopyable<Self>)
  func requiresCopyableT(_ t: AlwaysCopyable<T>)
  // expected-error@-1 {{type 'Self.T' does not conform to protocol 'Copyable'}}
}

protocol RemovedAgain where Self: ~Copyable {
    func requiresCopyableSelf(_ t: AlwaysCopyable<Self>) // expected-error {{type 'Self' does not conform to protocol 'Copyable'}}
}

struct StructContainment<T: ~Copyable> : Copyable {
    var storage: Maybe<T>
    // expected-error@-1 {{stored property 'storage' of 'Copyable'-conforming generic struct 'StructContainment' has non-Copyable type 'Maybe<T>'}}
}

enum EnumContainment<T: ~Copyable> : Copyable {
    // expected-note@-1 {{'T' has '~Copyable' constraint preventing implicit 'Copyable' conformance}}

    case some(T) // expected-error {{associated value 'some' of 'Copyable'-conforming generic enum 'EnumContainment' has non-Copyable type 'T'}}
    case other(Int)
    case none
}

class ClassContainment<T: ~Copyable> {
    var storage: T
    init(_ t: consuming T) {
        storage = t
        checkCopyable(t) // expected-error {{global function 'checkCopyable' requires that 'T' conform to 'Copyable'}}
    }

    deinit {}
}

struct ConditionalContainment<T: ~Copyable>: ~Copyable {
  var x: T
  var y: NC // expected-error {{stored property 'y' of 'Copyable'-conforming generic struct 'ConditionalContainment' has non-Copyable type 'NC'}}
}

extension ConditionalContainment: Copyable where T: Copyable {}

func chk(_ T: RequireCopyable<ConditionalContainment<Int>>) {}

func chk(_ t: ConditionalContainment<NC>) {}
// expected-error@-1 {{parameter of noncopyable type 'ConditionalContainment<NC>' must specify ownership}}
// expected-note@-2 3{{add}}

/// ----------------

// expected-note@+2 3{{add}}
// expected-error@+1 {{parameter of noncopyable type 'some Escapable & ~Copyable' must specify ownership}}
func dogDays(_ t: some Escapable & ~Copyable) {}

/// ----------------

struct AlwaysCopyableDeinit<T: ~Copyable> : Copyable {
  let nc: NC // expected-error {{stored property 'nc' of 'Copyable'-conforming generic struct 'AlwaysCopyableDeinit' has non-Copyable type 'NC'}}
  deinit {} // expected-error {{deinitializer cannot be declared in generic struct 'AlwaysCopyableDeinit' that conforms to 'Copyable'}}
}

struct SometimesCopyableDeinit<T: ~Copyable> : ~Copyable {
  deinit {} // expected-error {{deinitializer cannot be declared in generic struct 'SometimesCopyableDeinit' that conforms to 'Copyable'}}
}
extension SometimesCopyableDeinit: Copyable where T: Copyable {}

struct NeverCopyableDeinit<T: ~Copyable>: ~Copyable {
  deinit {}
}

protocol Test: ~Copyable {
  init?()
}

struct NoncopyableAndSendable: ~Copyable, Sendable {}

/// ---------------

// expected-note@+1 {{generic enum 'Maybe' has '~Copyable' constraint preventing 'Copyable' conformance}}
enum Maybe<Wrapped: ~Copyable>: ~Copyable {
  case just(Wrapped)
  case none

  deinit {} // expected-error {{deinitializer cannot be declared in generic enum 'Maybe' that conforms to 'Copyable'}}
}

extension Maybe: Copyable where Wrapped: Copyable {}

// expected-note@+4{{requirement specified as 'NC' : 'Copyable'}}
// expected-note@+3{{requirement from conditional conformance of 'Maybe<NC>' to 'Copyable'}}
// expected-note@+2{{requirement specified as 'Wrapped' : 'Copyable'}}
// expected-note@+1{{requirement from conditional conformance of 'Maybe<Wrapped>' to 'Copyable'}}
struct RequireCopyable<T> {
  // expected-note@-1 {{consider adding '~Copyable' to generic struct 'RequireCopyable'}}{{27-27=: ~Copyable}}
  deinit {} // expected-error {{deinitializer cannot be declared in generic struct 'RequireCopyable' that conforms to 'Copyable'}}
}

struct NC: ~Copyable {
// expected-note@-1 3{{struct 'NC' has '~Copyable' constraint preventing 'Copyable' conformance}}
  deinit {}
}

typealias ok1 = RequireCopyable<Int>
typealias ok2 = RequireCopyable<Maybe<Int>>

typealias err1 = RequireCopyable<Maybe<NC>>
// expected-error@-1{{type 'NC' does not conform to protocol 'Copyable'}}
// expected-error@-2{{'RequireCopyable' requires that 'NC' conform to 'Copyable'}}

typealias err2 = RequireCopyable<NC>
// expected-error@-1{{type 'NC' does not conform to protocol 'Copyable'}}

extension Maybe where Wrapped: ~Copyable {
  func check1(_ t: RequireCopyable<Self>) {}
  // expected-error@-1 {{type 'Wrapped' does not conform to protocol 'Copyable'}}
  // expected-error@-2 {{'RequireCopyable' requires that 'Wrapped' conform to 'Copyable'}}
}

extension Maybe {
  func check2(_ t: RequireCopyable<Self>) {}
}

/// MARK: tests that we diagnose ~Copyable that became invalid because it's required to be copyable

struct Silly: ~Copyable, Copyable {} // expected-error {{struct 'Silly' required to be 'Copyable' but is marked with '~Copyable'}}
enum Sally: Copyable, ~Copyable, NeedsCopyable {} // expected-error {{enum 'Sally' required to be 'Copyable' but is marked with '~Copyable'}}

class NiceTry: ~Copyable, Copyable {} // expected-error {{classes cannot be '~Copyable'}}

@_moveOnly class NiceTry2: Copyable {} // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}


struct Extendo: ~Copyable {}
extension Extendo: Copyable, ~Copyable {} // expected-error {{cannot suppress 'Copyable' in extension}}
// expected-error@-1 {{struct 'Extendo' required to be 'Copyable' but is marked with '~Copyable'}}

enum EnumExtendo {}
extension EnumExtendo: ~Copyable {} // expected-error {{cannot suppress 'Copyable' in extension}}

extension NeedsCopyable where Self: ~Copyable {}
// expected-error@-1 {{'Self' required to be 'Copyable' but is marked with '~Copyable'}}

protocol NoCopyP: ~Copyable {}

func needsCopyable<T>(_ t: T) {} // expected-note 2{{'where T: Copyable' is implicit here}}
func noCopyable(_ t: borrowing some ~Copyable) {}
func noCopyableAndP(_ t: borrowing some NoCopyP & ~Copyable) {}

func openingExistentials(_ a: borrowing any NoCopyP & ~Copyable,
                         _ b: any NoCopyP,
                         _ nc: borrowing any ~Copyable) {
  needsCopyable(a) // expected-error {{global function 'needsCopyable' requires that 'T' conform to 'Copyable'}}
  noCopyable(a)
  noCopyableAndP(a)

  needsCopyable(b)
  noCopyable(b)
  noCopyableAndP(b)

  needsCopyable(nc) // expected-error {{global function 'needsCopyable' requires that 'T' conform to 'Copyable'}}
  noCopyable(nc)
  noCopyableAndP(nc) // expected-error {{global function 'noCopyableAndP' requires that 'some NoCopyP & ~Copyable' conform to 'NoCopyP'}}
}

func project<CurValue>(_ base: CurValue) { }
func testSpecial(_ a: Any) {
  _openExistential(a, do: project)
}

/// MARK: non-Escapable types

func requireEscape<T: ~Copyable>(_ t: borrowing T) {} // expected-note {{generic parameters are always considered '@escaping'}}
// expected-note@-1 4{{'where T: Escapable' is implicit here}}

func genericNoEscape<T: ~Escapable>(_ t: borrowing T) {} // expected-note {{generic parameters are always considered '@escaping'}}
// expected-note@-1 2{{'where T: Copyable' is implicit here}}

func genericNoEscapeOrCopy<T: ~Escapable & ~Copyable>(_ t: borrowing T) {}

func checkFunctions(_ f: @autoclosure () -> Int) {
  requireEscape(f) // expected-error {{converting non-escaping parameter 'f' to generic parameter 'T' may allow it to escape}}

  // FIXME: rdar://119410346 (nonescaping function is not permitted as arg to ~Escapable generic function)
  genericNoEscape(f) // expected-error {{converting non-escaping parameter 'f' to generic parameter 'T' may allow it to escape}}
}

struct BuggerView<T: ~Copyable>: ~Escapable, Copyable {}

struct MutableBuggerView<T: ~Copyable>: ~Copyable, ~Escapable {}

func checkNominals(_ mutRef: inout MutableBuggerView<NC>,
                   _ ref: BuggerView<NC>,
                   _ intMutRef: borrowing MutableBuggerView<Int>,
                   _ intRef: BuggerView<Int>) {

  genericNoEscape(mutRef) // expected-error {{global function 'genericNoEscape' requires that 'MutableBuggerView<NC>' conform to 'Copyable'}}
  genericNoEscape(ref)
  genericNoEscape(intMutRef) // expected-error {{global function 'genericNoEscape' requires that 'MutableBuggerView<Int>' conform to 'Copyable'}}
  genericNoEscape(intRef)

  genericNoEscapeOrCopy(mutRef)
  genericNoEscapeOrCopy(ref)
  genericNoEscapeOrCopy(intMutRef)
  genericNoEscapeOrCopy(intRef)

  requireEscape(mutRef) // expected-error {{global function 'requireEscape' requires that 'MutableBuggerView<NC>' conform to 'Escapable'}}
  requireEscape(ref) // expected-error {{global function 'requireEscape' requires that 'BuggerView<NC>' conform to 'Escapable'}}
  requireEscape(intMutRef) // expected-error {{global function 'requireEscape' requires that 'MutableBuggerView<Int>' conform to 'Escapable'}}
  requireEscape(intRef) // expected-error {{global function 'requireEscape' requires that 'BuggerView<Int>' conform to 'Escapable'}}
}

struct NonescapingType: ~Escapable {}

struct NonescapeDoesNotAllowNoncopyable: ~Escapable { // expected-note {{consider adding '~Copyable' to struct 'NonescapeDoesNotAllowNoncopyable'}}
  let x: NC // expected-error {{stored property 'x' of 'Copyable'-conforming struct 'NonescapeDoesNotAllowNoncopyable' has non-Copyable type 'NC'}}
  init(_ x: borrowing NC) {
    self.x = x
  }
}

enum MaybeEscapes<T: ~Escapable>: ~Escapable { // expected-note {{generic enum 'MaybeEscapes' has '~Escapable' constraint preventing 'Escapable' conformance}}
  case just(T)
  case none
}

extension MaybeEscapes: Escapable where T: Escapable {}

struct Escapes { // expected-note {{consider adding '~Escapable' to struct 'Escapes'}}
  let t: MaybeEscapes<NonescapingType> // expected-error {{stored property 't' of 'Escapable'-conforming struct 'Escapes' has non-Escapable type 'MaybeEscapes<NonescapingType>'}}
}

enum Boring {
  case thing(MaybeEscapes<Int>)
}

struct NonEscapingHasNoDeinit: ~Escapable { // expected-note {{consider adding '~Copyable' to struct 'NonEscapingHasNoDeinit'}}
  deinit {} // expected-error {{deinitializer cannot be declared in struct 'NonEscapingHasNoDeinit' that conforms to 'Copyable'}}
}

/// MARK: requirement conflict tests

func conflict1<T>(_ t: T) where T: NeedsCopyable, T: ~Copyable {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

func conflict2<T: ~Copyable>(_ t: AlwaysCopyable<T>) {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

func conflict3a<T: NeedsCopyable & ~Copyable>(_ t: T) {}
// expected-error@-1 {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}

func conflict3b<T>(_ t: T) where T: NeedsCopyable, T: ~Copyable {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

func conflict4a(_ t: some NeedsCopyable & ~Copyable) {}
// expected-error@-1 {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}

protocol Conflict5: ~Copyable {
  borrowing func whatever() -> AlwaysCopyable<Self> // expected-error {{type 'Self' does not conform to protocol 'Copyable'}}
}

// expected-warning@+1 {{same-type requirement makes generic parameters 'U' and 'T' equivalent}}
func conflict6<T: ~Copyable, U>(_ t: T, _ u: U) // expected-error {{'T' required to be 'Copyable' but is marked with '~Copyable'}}
 where U : NeedsCopyable, T == U {}

protocol Conflict7 {
  associatedtype Element
}

func conflict7<T, U>(_ t: T, _ u: U)
  where
    U: ~Copyable,  // expected-error {{'U' required to be 'Copyable' but is marked with '~Copyable'}}
    T: Conflict7,
    U == T.Element
  {}

protocol Conflict8: ~Copyable, NeedsCopyable {}
// expected-error@-1 {{'Self' required to be 'Copyable' but is marked with '~Copyable'}}

struct Conflict9<T: NeedsCopyable> {}
func conflict9<U: ~Copyable>(_ u: Conflict9<U>) {}
// expected-error@-1 {{'U' required to be 'Copyable' but is marked with '~Copyable'}}

func conflict10<T>(_ t: T, _ u: some ~Copyable & Copyable)
// expected-error@-1 {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}
  where T: Copyable,
        T: ~Copyable {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

protocol Conflict11: ~Copyable, Copyable {}
// expected-error@-1 {{'Self' required to be 'Copyable' but is marked with '~Copyable'}}

struct Conflict12: ~Copyable, Copyable {}
// expected-error@-1 {{struct 'Conflict12' required to be 'Copyable' but is marked with '~Copyable'}}

// FIXME: this is bogus (rdar://119346022)
protocol Conflict13 {
  associatedtype A
  associatedtype B: ~Copyable
}
func conflict13<T>(_ t: T)
  where T: Conflict13,
        T.A == T.B
        {}

// expected-warning@+1 {{same-type requirement makes generic parameters 'U' and 'T' equivalent}}
func conflict14<T, U>(_ t: T, _ u: U)
  where T: ~Copyable, // expected-error {{'T' required to be 'Copyable' but is marked with '~Copyable'}}
        U: ~Escapable, // expected-error {{'U' required to be 'Escapable' but is marked with '~Escapable'}}
        T == U {}

protocol Conflict15 {
  associatedtype HasE: ~Copyable
  associatedtype HasC: ~Escapable
}
func conflict15<T, C, E>(_ t: T, _ c: C, _ e: borrowing E)
  where
    T: Conflict15,
    E: ~Copyable,  // expected-error {{'E' required to be 'Copyable' but is marked with '~Copyable'}}
    E == T.HasC,
    C: ~Escapable,  // expected-error {{'C' required to be 'Escapable' but is marked with '~Escapable'}}
    C == T.HasE
  {}


// Class bounds and AnyObject

class Soup {}
func checkClassBound1<T>(_ t: T) where T: ~Copyable, T: Soup {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

// expected-note@+2 3{{add}}
// expected-error@+1 {{parameter of noncopyable type 'T' must specify ownership}}
func checkClassBound2<T>(_ t: T) where T: ~Escapable, T: AnyObject, T: ~Copyable {}
// expected-error@-1 {{'T' required to be 'Escapable' but is marked with '~Escapable'}}
// expected-error@-2 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}

func checkClassBound3<T>(_ t: T) where T: Soup & ~Copyable & ~Escapable {}
// expected-error@-1 {{composition involving class requirement 'Soup' cannot contain '~Copyable'}}

func checkClassBound4<T>(_ t: T) where T: Soup, T: ~Copyable & ~Escapable {}
// expected-error@-1 {{'T' required to be 'Copyable' but is marked with '~Copyable'}}
// expected-error@-2 {{'T' required to be 'Escapable' but is marked with '~Escapable'}}

public func checkAnyObjInv1<Result: AnyObject>(_ t: borrowing Result) where Result: ~Copyable {}
// expected-error@-1 {{'Result' required to be 'Copyable' but is marked with '~Copyable'}}

public func checkAnyObjInv2<Result: AnyObject>(_ t: borrowing Result) where Result: ~Escapable {}
// expected-error@-1 {{'Result' required to be 'Escapable' but is marked with '~Escapable'}}

public func checkAnyObject<Result>(_ t: Result) where Result: AnyObject {
    checkCopyable(t)
}

func checkExistentialAndClasses(
    _ a: any AnyObject & ~Copyable, // expected-error {{composition involving 'AnyObject' cannot contain '~Copyable'}}
    _ b: any Soup & Copyable & ~Escapable & ~Copyable,
    // expected-error@-1 {{composition involving class requirement 'Soup' cannot contain '~Copyable'}}
    _ c: some (~Escapable & Removed) & Soup // expected-error {{composition cannot contain '~Escapable' when another member requires 'Escapable'}}
    ) {}

protocol HasNCBuddy: ~Copyable {
  associatedtype NCBuddy: HasNCBuddy, ~Copyable

  associatedtype Buddy: HasMember
}

protocol HasMember : HasNCBuddy {
  associatedtype Member: HasMember

  associatedtype NCMember: ~Copyable
}

func checkOwnership<T: HasMember>(_ t: T,
                                  _ l: T.NCBuddy.Buddy.Member.Buddy,
                                  _ m: T.Member.Member,
                                  _ n: T.Member.NCMember,
// expected-error@-1 {{parameter of noncopyable type 'T.Member.NCMember' must specify ownership}} // expected-note@-1 3{{add}}

                                  _ o: T.Member.NCBuddy.NCBuddy
// expected-error@-1 {{parameter of noncopyable type 'T.Member.NCBuddy.NCBuddy' must specify ownership}} // expected-note@-1 3{{add}}
) {}

// Covers an issue when building Combine from its interface.
public struct Record<Output> {
  public init(recording: Record<Output>) {}
}
protocol P {}
extension Record : Decodable where Output : P {}

struct Blahaj<Value>: ~Copyable {
  deinit {} // this is OK
}
extension Blahaj: Q where Value: Q {}
// expected-error@-1 {{type 'Blahaj<Value>' does not conform to protocol 'Copyable'}}
protocol Q: Copyable {}
// expected-note@-1 {{type 'Blahaj<Value>' does not conform to inherited protocol 'Copyable'}}

// expected-note@+2 3{{add}}
// expected-error@+1 {{parameter of noncopyable type 'Blahaj<T>' must specify ownership}}
func testBlahaj<T, U: Q>(_ x: Blahaj<T>,
// expected-note@+2 3{{add}}
// expected-error@+1 {{parameter of noncopyable type 'Blahaj<U>' must specify ownership}}
                         _ y: Blahaj<U>) {}

extension Int: NeedsCopyable {}

func checkExistentials() {
    let _: any ~Escapable & (NeedsCopyable & ~Copyable) // expected-error {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}
    let _: any NeedsCopyable & ~Copyable = 1 // expected-error {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}
    let _: any NeedsCopyable & ~Escapable = 1 // expected-error {{composition cannot contain '~Escapable' when another member requires 'Escapable'}}
    let _: any Copyable & ~Copyable = 1 // expected-error {{composition cannot contain '~Copyable' when another member requires 'Copyable'}}
    let _: any Escapable & ~Escapable = 1 // expected-error {{composition cannot contain '~Escapable' when another member requires 'Escapable'}}
}

typealias NotCopyable = ~Copyable
typealias EmptyComposition = ~Copyable & ~Escapable
func test(_ t: borrowing NotCopyable) {} // expected-error {{use of 'NotCopyable' (aka '~Copyable') as a type must be written 'any NotCopyable'}}
func test(_ t: borrowing EmptyComposition) {} // expected-error {{use of 'EmptyComposition' (aka '~Copyable & ~Escapable') as a type must be written 'any EmptyComposition' (aka 'any ~Copyable & ~Escapable')}}

typealias Copy = Copyable
func test(_ z1: Copy, _ z2: Copyable) {}

// Conformances can be conditional on whether a generic parameter is Copyable
protocol Arbitrary {}
protocol AnotherOne {}
struct UnethicalPointer<Pointee: ~Copyable> {}
extension UnethicalPointer: Arbitrary {}
extension UnethicalPointer: AnotherOne where Pointee: Copyable {}

struct AlsoLegal1<Pointee: ~Escapable> {}
extension AlsoLegal1: Arbitrary {}
extension AlsoLegal1: AnotherOne where Pointee: Escapable {}

struct SillIllegal2<Pointee> {}
extension SillIllegal2: Arbitrary where Pointee: Sendable {}
// expected-error@-1 {{conditional conformance to non-marker protocol 'Arbitrary' cannot depend on conformance of 'Pointee' to marker protocol 'Sendable'}}

struct SSS: ~Copyable, PPP {}
protocol PPP: ~Copyable {}
let global__old__: any PPP = SSS() // expected-error {{value of type 'SSS' does not conform to specified type 'Copyable'}}
let global__new__: any PPP & ~Copyable = SSS()

struct Example<T> {}

struct TestResolution1 { // expected-note {{consider adding '~Copyable' to struct 'TestResolution1'}}
  var maybeNC: NC? = nil // expected-error {{stored property 'maybeNC' of 'Copyable'-conforming struct 'TestResolution1' has non-Copyable type 'NC?'}}
}

struct TestResolution2 { // expected-note {{consider adding '~Copyable' to struct 'TestResolution2'}}
  var maybeIOUNC: NC! = nil // expected-error {{stored property 'maybeIOUNC' of 'Copyable'-conforming struct 'TestResolution2' has non-Copyable type 'NC?'}}
}

struct TestResolution3 {
  var arrayNC: [NC] = [] // expected-error {{type 'NC' does not conform to protocol 'Copyable'}}
  var dictNC: [String: NC] = [:] // expected-error {{type 'NC' does not conform to protocol 'Copyable'}}
  var exampleNC: Example<NC> = Example() // expected-error {{type 'NC' does not conform to protocol 'Copyable'}}
}

public struct Box<Wrapped: ~Copyable>: ~Copyable {}
// Box is never copyable, so we can't support this conditional conformance.
public enum List<Element: ~Copyable>: ~Copyable {
  case cons(Element, Box<List<Element>>)   // expected-error {{associated value 'cons' of 'Copyable'-conforming generic enum 'List' has non-Copyable type '(Element, Box<List<Element>>)'}}
  case empty
}
extension List: Copyable where Element: Copyable {}


struct Yapping<T: ~Copyable> {}
extension Yapping { // expected-note {{'where T: Copyable' is implicit here}}
  func yap() {}
}
func testYap(_ y: Yapping<NC>) {
  y.yap() // expected-error {{referencing instance method 'yap()' on 'Yapping' requires that 'NC' conform to 'Copyable'}}
}

protocol Veggie: ~Copyable {}
func generalized(_ x: Any.Type) {}
func testMetatypes(_ t: (any Veggie & ~Copyable).Type) {
  generalized(t) // expected-error {{cannot convert value of type '(any Veggie & ~Copyable).Type' to expected argument type 'any Any.Type'}}
}
