// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=BAD_MEMBERS_1 | %FileCheck %s -check-prefix=BAD_MEMBERS_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=BAD_MEMBERS_2 | %FileCheck %s -check-prefix=BAD_MEMBERS_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=CLOSURE_CALLED_IN_PLACE_1 | %FileCheck %s -check-prefix=WITH_GLOBAL_INT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_28991372 | %FileCheck %s -check-prefix=RDAR_28991372

class BadMembers1 {
  var prop: Int {
    get {}
    a
  }
  subscript(i: Int) -> Double {
  }
}
func badMembers1(_ a: BadMembers1) {
  a#^BAD_MEMBERS_1^#
}
// BAD_MEMBERS_1-DAG: Decl[InstanceVar]/CurrNominal: .prop[#Int#]{{; name=.+$}}
// BAD_MEMBERS_1-DAG: Decl[Subscript]/CurrNominal:   [{#(i): Int#}][#Double#]{{; name=.+$}}

protocol BadMembers2 {
  var prop: Int {
    get {}
    a
  }
  subscript(i: Int) -> Double {
  }
}
func badMembers2(_ a: BadMembers2) {
  a#^BAD_MEMBERS_2^#
}
// BAD_MEMBERS_2: Begin completions, 3 items
// BAD_MEMBERS_2-DAG: Decl[InstanceVar]/CurrNominal: .prop[#Int#]{{; name=.+$}}
// BAD_MEMBERS_2-DAG: Decl[Subscript]/CurrNominal:   [{#(i): Int#}][#Double#]{{; name=.+$}}
// BAD_MEMBERS_2-DAG: Keyword[self]/CurrNominal:     .self[#any BadMembers2#]; name=self

func globalFunc() {}

func globalFuncInt() -> Int { return 0 }

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=LET_COMPUTED | %FileCheck %s -check-prefix=WITH_GLOBAL
class C {
  let x : Int { #^LET_COMPUTED^# }
}

// WITH_GLOBAL-DAG: Decl[FreeFunction]/CurrModule: globalFunc()[#Void#]; name=globalFunc()

({ x in 2+x })(#^CLOSURE_CALLED_IN_PLACE_1^#

// WITH_GLOBAL_INT-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Convertible]: globalFuncInt()[#Int#]; name=globalFuncInt()

// rdar://19634354
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_19634354
while true {
  func f() {
    a#^RDAR_19634354^#
  }
}

// rdar://problem/21197042
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GENERIC_PARAM_AND_ASSOC_TYPE | %FileCheck %s -check-prefix=GENERIC_PARAM_AND_ASSOC_TYPE
struct CustomGenericCollection<Key> : ExpressibleByDictionaryLiteral {
  // GENERIC_PARAM_AND_ASSOC_TYPE-DAG: Decl[InstanceVar]/CurrNominal/NotRecommended/TypeRelation[Convertible]:      count[#Int#]; name=count
  // GENERIC_PARAM_AND_ASSOC_TYPE-DAG: Decl[GenericTypeParam]/Local:       Key[#Key#]; name=Key
  // GENERIC_PARAM_AND_ASSOC_TYPE-DAG: Decl[TypeAlias]/CurrNominal:        Value[#Value#]; name=Value

  var count: Int { #^GENERIC_PARAM_AND_ASSOC_TYPE^# }
}

// rdar://problem/21796881
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_21796881
extension ExpressibleByNilLiteral {
   var nil: Self { #^RDAR_21796881^# }
}

// rdar://problem/21436558
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_21436558
private protocol RoundRobin : Sendable, Receivable {
  typealias _NEXT
}
#if TESTING
  extension RoundRobinAS {
    mutating func appendNextTo(_ acceptor:#^RDAR_21436558^#
  }
#endif

// rdar://problem/21435993
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_21435993
class C<T> {
  func test() {
    do {} catch { #^RDAR_21435993^# }
  }
  func accidentallyNested<U>(_ x: U) {}
}

// rdar://problem/21149908
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_21149908
@objc func handleTap(_ recognizer: UIGestureRecognizer) {
  if recognizer.state == .Ended {
    let _ : () = self.suggestion.cata(#^RDAR_21149908^#{ _ in
      listView?.backUpOneGroup()
      }, right: { _ in
        listView?.handleCompletion(self)
    }
  }
}

// rdar://problem/22036358
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22036358
public extension AnyIterator {
  public extension AnySequence {
    public func take(_ n: Int) -> AnySequence<Element> {
      var xs: [Element] = []
      #^RDAR_22036358^#
      return AnySequence(xs)
    }
  }
}

// rdar://problem/22012123
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22012123
protocol Fooable {
  protocol FooableZingable : Fooable {
    #^RDAR_22012123^#
  }
}

// rdar://problem/22688199
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22688199
func curried(_ a: Int)(_ b1: Int, _ b2: Int) { }
func flip<A, B, C>(_ f: A -> B -> C) -> B -> A -> C { }
func rdar22688199() {
  let f = flip(curried)(#^RDAR_22688199^#
}

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22836263
func rdar22836263() {
  let x: [Int]
  nosuchfunc(x[0].#^RDAR_22836263^#)
}

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22835966
func rdar22835966() {
  class Inner {
    var prop = #^RDAR_22835966^#
  }
}

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22834017 | %FileCheck %s -check-prefix=RDAR_22834017
struct Foo {
  let a: Anosuchtype
  let b: Bnosuchtype
  let c: Cnosuchtype
}

func rdar22834017() {
  Foo(#^RDAR_22834017^#)
}
// RDAR_22834017: Begin completions, 1 items
// RDAR_22834017: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#a: _#}, {#b: _#}, {#c: _#}[')'][#Foo#];

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_23173692 | %FileCheck %s -check-prefix=RDAR_23173692
func rdar23173692() {
  return IndexingIterator(#^RDAR_23173692^#)
}
// RDAR_23173692: Begin completions

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_22769393 | %FileCheck %s -check-prefix=RDAR_22769393
public enum PropertyListItem {
  case PLString(String)
  case PLDict([String:PropertyListItem])
}
class Connection {
  var handler: (Int32, [UInt8]) -> () = { _ in }
}
private let conn = Connection()
conn.handler = { (msgID, msg) in
  // Otherwise, we should have a structured message.
  let info = { () -> PropertyListItem in }()
  guard case .PLDict(var infoItems) = info else { fatalError("invalid message") }
  guard case .Some(.PLString(let command)) = infoItems["command"] else { fatalError("invalid message") }
  switch command {
  case "listSessions":
    var items = #^RDAR_22769393^#
  default:
    break
  }
}
// RDAR_22769393: Begin completions

struct S_RDAR_28991372 {
  init(x: Int, y: Int) {}
}

S_RDAR_28991372(x: #^RDAR_28991372^#, y: <#T##Int#>)
// RDAR_28991372: Begin completions

// rdar://problem/31981486
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_31981486 | %FileCheck %s -check-prefix=RDAR_31981486

protocol P where #^RDAR_31981486^#
// RDAR_31981486: Begin completions

// rdar://problem/38149042
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_38149042 | %FileCheck %s -check-prefix=RDAR_38149042
class Baz_38149042 {
  let x: Int = 0
}
protocol Bar_38149042 {
  var foo: Baz_38149042? {get}
}
func foo_38149042(bar: Bar_38149042) {
  _ = bar.foo? #^RDAR_38149042^# .x
}
// RDAR_38149042-DAG: Decl[InstanceVar]/CurrNominal:                  .x[#Int#]; name=x
// RDAR_38149042-DAG: Keyword[self]/CurrNominal: .self[#Baz_38149042#]; name=self

// rdar://problem/38272904
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=RDAR_38272904 | %FileCheck %s -check-prefix=RDAR_38272904
public protocol P_38272904 {
    associatedtype A = Error
    associatedtype B
}

public func ?? <T: P_38272904>(lhs: T, rhs: @autoclosure() throws -> T.B) rethrows -> T.B {
  fatalError()
}

class A_38272904 {
  open class func foo() -> A_38272904 { fatalError() }
}

func bar_38272904(a: A_38272904) {}
func foo_38272904(a: A_38272904) {
  bar_38272904(a: .foo() #^RDAR_38272904^#)
}
// RDAR_38272904: Begin completions

// rdar://problem/41159258
// RUN: %target-swift-ide-test -code-completion -source-filename=%s -code-completion-token=RDAR41159258_1 | %FileCheck %s -check-prefix=RDAR_41159258
// RUN: %target-swift-ide-test -code-completion -source-filename=%s -code-completion-token=RDAR41159258_2
public func ==(lhs: RDAR41159258_MyResult1, rhs: RDAR41159258_MyResult1) -> Bool {
  fatalError()
}
public func ==(lhs: RDAR41159258_MyResult2, rhs: RDAR41159258_MyResult2) -> Bool {
  fatalError()
}
public enum RDAR41159258_MyResult1 {
  case failure(Error)
}
public enum RDAR41159258_MyResult2 {
  case failure(Error)
}

public struct RDAR41159258_MyError: Error {}

func foo(x: RDAR41159258_MyResult1) {
  let x: RDAR41159258_MyResult1
  x = .failure(RDAR41159258_MyError()) #^RDAR41159258_1^#
  let y: Bool
  y = .failure(RDAR41159258_MyError()) #^RDAR41159258_2^#
}
// RDAR_41159258: Begin completions


// rdar://problem/41232519
// RUN: %target-swift-ide-test -code-completion -source-filename=%s -code-completion-token=RDAR41232519_1 | %FileCheck %s -check-prefix=RDAR_41232519
// RUN: %target-swift-ide-test -code-completion -source-filename=%s -code-completion-token=RDAR41232519_2 | %FileCheck %s -check-prefix=RDAR_41232519
public protocol IntProvider {
  func nextInt() -> Int
}

public final class IntStore {
  public var myInt: Int = 0
  func readNextInt(from provider: IntProvider) {
      myInt = provider.nextInt() #^RDAR41232519_1^#
      _ = true ? 1 : provider.nextInt() #^RDAR41232519_2^#
  }
}
// RDAR_41232519: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem/TypeRelation[Convertible]: [' ']+ {#Int#}[#Int#]; name=+

// rdar://problem/28188259
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_28188259 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_28188259
func test_28188259(x: ((Int) -> Void) -> Void) {
  x({_ in }#^RDAR_28188259^#)
}
// RDAR_28188259-DAG: Pattern/CurrModule/Flair[ArgLabels]/TypeRelation[Invalid]: ({#_#})[#Void#]; name=()
// RDAR_28188259-DAG: Keyword[self]/CurrNominal:          .self[#(_) -> ()#]; name=self

// rdar://problem/40956846
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_40956846 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_40956846
func test_40956846(
  arg_40956846_1: inout Int!,
  arg_40956846_2: Void!,
  arg_40956846_3: (() -> Int?)!,
  arg_40956846_4: inout ((Int) -> Int)!
) {
  let y = #^RDAR_40956846^#
}
// RDAR_40956846-DAG: Decl[LocalVar]/Local:               arg_40956846_1[#inout Int!#]; name=arg_40956846_1
// RDAR_40956846-DAG: Decl[LocalVar]/Local:               arg_40956846_2[#Void!#]; name=arg_40956846_2
// RDAR_40956846-DAG: Decl[LocalVar]/Local:               arg_40956846_3[#(() -> Int?)!#]; name=arg_40956846_3
// RDAR_40956846-DAG: Decl[LocalVar]/Local:               arg_40956846_4[#inout ((Int) -> Int)!#]; name=arg_40956846_4

// rdar://problem/42443512
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_42443512 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_42443512
class test_42443512 {
  func foo(x: Int!) { }
  static func test() {
    self.foo#^RDAR_42443512^#
  }
}
// RDAR_42443512: Begin completions

// rdar://problem/42452085
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_42452085_1 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_42452085
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_42452085_2 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_42452085
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_42452085_3 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_42452085
class cls_42452085 {
  var value: Any
  func canThrow() throws -> Int { return 1 }
}
func test_42452085(any: Any, obj: cls_42452085?) throws {
  var object: Any? = nil
  object = (any as? String) #^RDAR_42452085_1^#
  obj?.value = any #^RDAR_42452085_2^#
  _ = try obj?.canThrow() #^RDAR_42452085_3^#
}
// RDAR_42452085: Begin completions

// rdar://problem/41234606
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_41234606 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_41234606
#if false
extension Foo {
  func foo<T: Collection #^RDAR_41234606^#>(x: T) {}
}
#endif
// RDAR_41234606: Begin completion
// RDAR_41234606-DAG: Decl[AssociatedType]/CurrNominal/IsSystem: .Element; name=Element
// RDAR_41234606-DAG: Decl[AssociatedType]/CurrNominal/IsSystem: .Iterator; name=Iterator

// rdar://problem/41071587
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_41071587 -source-filename=%s
func test_41071587(x: Any) {
  switch x {
    case (let (_, _)) #^RDAR_41071587^#:
      ()
  }
}

// rdar://problem/54215016
// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_54215016 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_54215016
struct test_54215016 {
  func genericError<Value>()
  func test() {
    genericError(#^RDAR_54215016^#)
// RDAR_54215016: Begin completions
  }
}

// RUN: %target-swift-ide-test -code-completion -code-completion-token=CRASH_CALL_AS_FUNCTION -source-filename=%s | %FileCheck %s -check-prefix=CRASH_CALL_AS_FUNCTION
protocol HasCallAsFunctionRequirement {
  func callAsFunction()
}
struct StructWithCallAsFunction: HasCallAsFunctionRequirement {
  let f = #^CRASH_CALL_AS_FUNCTION^#
  func callAsFunction() {}
}
// CRASH_CALL_AS_FUNCTION: Begin completion

// rdar://80635105
protocol P_80635105 {
  associatedtype T
}
struct S_80635105<T> {}
extension S_80635105 : P_80635105 {}
extension P_80635105 {
  func foo<U : P_80635105>(_ x: U.T) where U == Self.T {}
}

// RUN: %target-swift-ide-test -code-completion -code-completion-token=RDAR_80635105 -source-filename=%s | %FileCheck %s -check-prefix=RDAR_80635105
func test_80635105() {
  let fn = { x in
    S_80635105.#^RDAR_80635105^#
    // RDAR_80635105: Decl[InstanceMethod]/Super: foo({#(self): S_80635105<_>#})[#(P_80635105.T) -> Void#]; name=foo
  }
}
