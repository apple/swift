// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

struct S {}
postfix operator ++ {}
postfix func ++(x: inout S) -> S { return x }

func testPostfix1(x: S) {
  x#^POSTFIX_1^#
}
// POSTFIX_1-NOT: ++

func testPostfix2(x: inout S) {
  x#^POSTFIX_2?check=POSTFIX_2;check=NEGATIVE_POSTFIX_2^#
}
// POSTFIX_2-DAG: Decl[PostfixOperatorFunction]/CurrModule:  ++[#S#]
// NEGATIVE_POSTFIX_2-NOT: --


postfix operator +- {}
postfix func +-(x: S) -> S? { return x }
func testPostfix3(x: S) {
  x#^POSTFIX_3^#
}
// POSTFIX_3: Decl[PostfixOperatorFunction]/CurrModule:  +-[#S?#]

func testPostfix4(x: S?) {
  x#^POSTFIX_4^#
}
// POSTFIX_4: BuiltinOperator/None:  ![#S#]

struct T {}
postfix func +-<G>(x: [G]) -> G { return x! }
func testPostfix5(x: [T]) {
  x#^POSTFIX_5^#
}
// POSTFIX_5: Decl[PostfixOperatorFunction]/CurrModule:  +-[#T#]

protocol Fooable {}
extension Int : Fooable {}
extension Double : Fooable {}

postfix operator *** {}
postfix func ***<G: Fooable>(x: G) -> G { return x }
func testPostfix6() {
  1 + 2 * 3#^POSTFIX_6^#
}
// POSTFIX_6: Decl[PostfixOperatorFunction]/CurrModule/TypeRelation[Convertible]:  ***[#Int#]

func testPostfix7() {
  1 + 2 * 3.0#^POSTFIX_7^#
}
// POSTFIX_7: Decl[PostfixOperatorFunction]/CurrModule:  ***[#Double#]

func testPostfix8(x: S) {
  x#^POSTFIX_8^#
}
// POSTFIX_8: Keyword[self]/CurrNominal: .self[#S#]; name=self

protocol P {
  associatedtype T
  func foo() -> T
}

func testPostfix9<G: P where G.T == Int>(x: G) {
  x.foo()#^POSTFIX_9^#
}
// POSTFIX_9: Decl[PostfixOperatorFunction]/CurrModule: ***[#Int#]

func testPostfix10<G: P where G.T : Fooable>(x: G) {
  x.foo()#^POSTFIX_10^#
}
// POSTFIX_10: Decl[PostfixOperatorFunction]/CurrModule: ***[#G.T#]

func testPostfixSpace(x: inout S) {
  x #^S_POSTFIX_SPACE^#
}
// S_POSTFIX_SPACE: Decl[PostfixOperatorFunction]/CurrModule/Erase[1]:  ++[#S#]


// ===--- Infix operators

precedencegroup S2PrecedenceGroup {
  associativity: left
  lowerThan: ComparisonPrecedence
  higherThan: AssignmentPrecedence
}
precedencegroup S2AssignmentPrecedenceGroup {
  associativity: none
  lowerThan: ComparisonPrecedence
  higherThan: AssignmentPrecedence
}

struct S2 {}
infix operator ** : S2PrecedenceGroup
infix operator **= : S2AssignmentPrecedenceGroup
func +(x: S2, y: S2) -> S2 { return x }
func **(x: S2, y: Int) -> S2 { return x }
func **=(x: inout S2, y: Int) -> Void { return x }

func testInfix1(x: S2) {
  x#^INFIX_1?check=S2_INFIX;check=NEGATIVE_S2_INFIX^#
}
// FIXME: rdar://problem/22997089 - should be CurrModule
// S2_INFIX-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:   + {#S2#}[#S2#]
// S2_INFIX-DAG: Decl[InfixOperatorFunction]/CurrModule:   ** {#Int#}[#S2#]; name=**
// NEGATIVE_S2_INFIX-NOT: **=
// NEGATIVE_S2_INFIX-NOT: +=
// NEGATIVE_S2_INFIX-NOT: \* {#Int#}
// NEGATIVE_S2_INFIX-NOT: ??
// NEGATIVE_S2_INFIX-NOT: ~=
// NEGATIVE_S2_INFIX-NOT: ~>
// NEGATIVE_S2_INFIX-NOT: = {#

func testInfix2(x: inout S2) {
  x#^INFIX_2?check=S2_INFIX_LVALUE;check=NEGATIVE_S2_INFIX_LVALUE^#
}
// FIXME: rdar://problem/22997089 - should be CurrModule
// S2_INFIX_LVALUE-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:   + {#S2#}[#S2#]
// S2_INFIX_LVALUE-DAG: Decl[InfixOperatorFunction]/CurrModule:   ** {#Int#}[#S2#]
// S2_INFIX_LVALUE-DAG: Decl[InfixOperatorFunction]/CurrModule:   **= {#Int#}[#Void#]
// S2_INFIX_LVALUE-DAG: BuiltinOperator/None:                             = {#S2#}
// NEGATIVE_S2_INFIX_LVALUE-NOT: +=
// NEGATIVE_S2_INFIX_LVALUE-NOT: \* {#Int#}
// NEGATIVE_S2_INFIX_LVALUE-NOT: ??
// NEGATIVE_S2_INFIX_LVALUE-NOT: ~=
// NEGATIVE_S2_INFIX_LVALUE-NOT: ~>

func testInfix3(x: inout S2) {
  x#^INFIX_3?check=S2_INFIX_LVALUE^#
}

func testInfix4() {
  S2()#^INFIX_4?check=S2_INFIX^#
}

func testInfix5() {
  (S2() + S2())#^INFIX_5?check=S2_INFIX^#
}

func testInfix6<T: P where T.T == S2>(x: T) {
  x.foo()#^INFIX_6?check=S2_INFIX^#
}

func testInfix7(x: S2?) {
  x#^INFIX_7?check=S2_INFIX_OPTIONAL;check=NEGATIVE_S2_INFIX_OPTIONAL^#
}
// S2_INFIX_OPTIONAL-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  != {#{{.*}}#}[#Bool#]
// S2_INFIX_OPTIONAL-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#{{.*}}#}[#Bool#]
// S2_INFIX_OPTIONAL-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  ?? {#S2#}[#S2#]; name=??
// The equality operators don't come from equatable.
// NEGATIVE_S2_INFIX_OPTIONAL-NOT: == {#S2

struct S3: Equatable {}
func ==(x: S3, y: S3) -> Bool { return true }
func !=(x: S3, y: S3) -> Bool { return false}

func testInfix8(x: S3?) {
  x#^INFIX_8?check=S3_INFIX_OPTIONAL^#
}
// The equality operators come from equatable.
// S3_INFIX_OPTIONAL-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#S3?#}[#Bool#]

infix operator **** {
  associativity left
  precedence 123
}
func ****<T: Fooable>(x: T, y: T) -> T { return x }

func testInfix9<T: P where T.T: Fooable>(x: T) {
  x.foo()#^INFIX_9?check=FOOABLE_INFIX^#
}
// FOOABLE_INFIX: Decl[InfixOperatorFunction]/CurrModule:   **** {#T.T#}[#T.T#]

func testInfix10<T: P where T.T: Fooable>(x: T) {
  (x.foo() **** x.foo())#^INFIX_10?check=FOOABLE_INFIX^#
}

func testInfix11() {
  S2#^INFIX_11^#
}

// INFIX_11: Begin completions, 3 items
// INFIX_11-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ()[#S2#]; name=()
// INFIX_11-DAG: Keyword[self]/CurrNominal:          .self[#S2.Type#]; name=self
// INFIX_11-DAG: Keyword/CurrNominal:                .Type[#S2.Type#]; name=Type

func testInfix12() {
  P#^INFIX_12^#
}
// INFIX_12: Begin completions, 4 items
// INFIX_12-DAG: Decl[AssociatedType]/CurrNominal:   .T; name=T
// INFIX_12-DAG: Keyword[self]/CurrNominal:          .self[#(any P).Type#]; name=self
// INFIX_12-DAG: Keyword/CurrNominal:                .Protocol[#(any P).Type#]; name=Protocol
// INFIX_12-DAG: Keyword/CurrNominal:                .Type[#any P.Type#]; name=Type

func testInfix13() {
  P.foo#^INFIX_13?check=NO_OPERATORS^#
}
// NO_OPERATORS-NOT: Decl[InfixOperatorFunction]

func testInfix14() {
  P.T#^INFIX_14?check=NO_OPERATORS^#
}
func testInfix15<T: P where T.T == S2>() {
  T#^INFIX_15^#
}
// INFIX_15: Begin completions, 6 items
// INFIX_15-DAG: Decl[AssociatedType]/CurrNominal:   .T; name=T
// INFIX_15-DAG: Decl[InstanceMethod]/CurrNominal:   .foo({#(self): P#})[#() -> S2#]; name=foo(:)
// INFIX_15-DAG: Keyword[self]/CurrNominal:          .self[#T.Type#]; name=self
// INFIX_15-DAG: Keyword/CurrNominal:                .Type[#T.Type#]; name=Type
// INFIX_15-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  != {#(any Any.Type)?#}[#Bool#];
// INFIX_15-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#(any Any.Type)?#}[#Bool#];

func testInfix16<T: P where T.T == S2>() {
  T.foo#^INFIX_16^#
}

// INFIX_16: Begin completions, 2 items
// INFIX_16-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(self): P#})[#() -> S2#]; name=(:)
// INFIX_16-DAG: Keyword[self]/CurrNominal:        .self[#(T) -> () -> S2#]; name=self

func testInfix17(x: Void) {
  x#^INFIX_17?check=VOID_OPERATORS^#
}

// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  != {#()#}[#Bool#]; name=!=
// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#()#}[#Bool#]; name===
// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  <= {#()#}[#Bool#]; name=<=
// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  >= {#()#}[#Bool#]; name=>=
// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  < {#()#}[#Bool#]; name=<
// VOID_OPERATORS-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  > {#()#}[#Bool#]; name=>

func testInfix18(x: (S2, S2) {
  x#^INFIX_18?check=NO_OPERATORS^#
}
class EmptyClass {}
func testInfix19(x: EmptyClass) {
  x#^INFIX_19?check=EMPTYCLASS_INFIX^#
}

// EMPTYCLASS_INFIX-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: === {#AnyObject?#}[#Bool#]
// EMPTYCLASS_INFIX-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: !== {#AnyObject?#}[#Bool#]

enum E {
  case A
  case B(S2)
}
func testInfix20(x: E) {
  x#^INFIX_20?check=NO_OPERATORS^#
}
func testInfix21() {
  E.A#^INFIX_21?check=NO_OPERATORS^#
}
func testInfix22() {
  E.B#^INFIX_22^#
}
// INFIX_22: Begin completions, 2 items
// INFIX_22-DAG: Pattern/CurrModule/Flair[ArgLabels]:               ({#S2#})[#E#]; name=()

func testSpace(x: S2) {
  x #^S2_INFIX_SPACE^#
}
// S2_INFIX_SPACE-DAG: Decl[InfixOperatorFunction]/CurrModule: [' ']** {#Int#}[#S2#]
// S2_INFIX_SPACE-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']+ {#S2#}[#S2#]

func testExtInfix1(x: inout S2) {
  x + S2() + x + S2() + x + S2() + x#^EXT_INFIX_1^#
}
// EXT_INFIX_1: Begin completions
// EXT_INFIX_1-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]: ** {#Int#}[#S2#]
// EXT_INFIX_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem/TypeRelation[Convertible]: + {#S2#}[#S2#]
// EXT_INFIX_1: End completions

struct S4 {}
func +(x: S4, y: S4) -> S4 { return x }
func ==(x: S4, y: S4) -> Bool { return true }

infix operator +++ : ReallyLowPrecedence
precedencegroup ReallyLowPrecedence {
  associativity: left
  lowerThan: AssignmentPrecedence
}
func +++(x: S4, y: S4) -> S4 { return x }
infix operator &&& : ReallyHighPrecedence
precedencegroup ReallyHighPrecedence {
  associativity: left
  higherThan: BitwiseShiftPrecedence
}
func &&&(x: Bool, y: Bool) -> S4 { return x }

func testExtInfix2(x: S4) {
  x + x == x + x#^EXT_INFIX_2^#
}
// EXT_INFIX_2: Begin completions, 4 items
// EXT_INFIX_2-DAG: Keyword[self]/CurrNominal:          .self[#S4#];
// EXT_INFIX_2-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]:  +++ {#S4#}[#S4#];
// EXT_INFIX_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem/TypeRelation[Convertible]:  + {#S4#}[#S4#];
// EXT_INFIX_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#S4#}[#Bool#];

func testExtInfix3(x: S4) {
   x + x#^EXT_INFIX_3^#
}
// EXT_INFIX_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem/TypeRelation[Convertible]:  + {#S4#}[#S4#]
// EXT_INFIX_3-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]:  +++ {#S4#}[#S4#]

func testExtInfix4(x: S4) {
   1 + 1.0 + x#^EXT_INFIX_4^#
}
// EXT_INFIX_4: Begin completions
// EXT_INFIX_4-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  + {#S4#}[#S4#]
// EXT_INFIX_4-DAG: Decl[InfixOperatorFunction]/CurrModule:  +++ {#S4#}[#S4#]
// EXT_INFIX_4: End completions

func testAssignTuple1() {
  ()#^ASSIGN_TUPLE_1^#
}
func testAssignTuple3() {
  func void() {}
  void()#^ASSIGN_TUPLE_3?check=ASSIGN_TUPLE_1^#
}
// FIXME: technically this is sometimes legal, but we would need to
// differentiate between cases like () = and print() =. Since it's not very
// useful anyway, just omit the completion.
// ASSIGN_TUPLE_1-NOT: BuiltinOperator/None:  = {

func testAssignTuple2() {
  var x: S2
  var y: S2
  (x, y)#^ASSIGN_TUPLE_2^#
}
// ASSIGN_TUPLE_2: BuiltinOperator/None:                        = {#(S2, S2)#};


infix operator ====: ComparisonPrecedence
infix operator &&&& : LogicalConjunctionPrecedence
infix operator |||| : LogicalDisjunctionPrecedence
struct Boolish {}
func ====(x: Boolish, y: Boolish) -> Boolish { return x }
func &&&&(x: Boolish, y: @autoclosure ()->Boolish) -> Boolish { return x }
func ||||(x: Boolish, y: @autoclosure ()->Boolish) -> Boolish { return x }

func testAutoclosure(x: Boolish, y: Boolish) {
  if x #^INFIX_AUTOCLOSURE_1^# {}
  if x &&&& y #^INFIX_AUTOCLOSURE_2^# {}
  if x |||| y #^INFIX_AUTOCLOSURE_3?check=INFIX_AUTOCLOSURE_2^# {}
  if x &&&& x |||| y #^INFIX_AUTOCLOSURE_4?check=INFIX_AUTOCLOSURE_2^# {}
}

// INFIX_AUTOCLOSURE_1-DAG: Decl[InfixOperatorFunction]/CurrModule: [' ']&&&& {#Boolish#}[#Boolish#];
// INFIX_AUTOCLOSURE_1-DAG: Decl[InfixOperatorFunction]/CurrModule: [' ']==== {#Boolish#}[#Boolish#];
// INFIX_AUTOCLOSURE_1-DAG: Decl[InfixOperatorFunction]/CurrModule: [' ']|||| {#Boolish#}[#Boolish#];

// INFIX_AUTOCLOSURE_2: Begin completions
// INFIX_AUTOCLOSURE_2-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]: [' ']&&&& {#Boolish#}[#Boolish#];
// INFIX_AUTOCLOSURE_2-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]: [' ']==== {#Boolish#}[#Boolish#];
// INFIX_AUTOCLOSURE_2-DAG: Decl[InfixOperatorFunction]/CurrModule/TypeRelation[Convertible]: [' ']|||| {#Boolish#}[#Boolish#];
// INFIX_AUTOCLOSURE_2: End completions

