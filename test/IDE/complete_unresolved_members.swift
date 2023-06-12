// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s %S/Inputs/EnumFromOtherFile.swift -filecheck %raw-FileCheck -completion-output-dir %t

// NOCRASH: Token

enum SomeEnum1 {
  case South
  case North
}

enum SomeEnum2 {
  case East
  case West
}

enum SomeEnum3: Hashable {
  case Payload(SomeEnum1)
}

struct NotOptions1 {
  static let NotSet = 1
}

struct SomeOptions1 : OptionSet {
  let rawValue : Int
  static let Option1 = SomeOptions1(rawValue: 1 << 1)
  static let Option2 = SomeOptions1(rawValue: 1 << 2)
  static let Option3 = SomeOptions1(rawValue: 1 << 3)
  let NotStaticOption = SomeOptions1(rawValue: 1 << 4)
  static let NotOption = 1
}

struct SomeOptions2 : OptionSet {
  let rawValue : Int
  static let Option4 = SomeOptions2(rawValue: 1 << 1)
  static let Option5 = SomeOptions2(rawValue: 1 << 2)
  static let Option6 = SomeOptions2(rawValue: 1 << 3)
}

enum EnumAvail1 {
  case aaa
  @available(*, unavailable) case AAA
  @available(*, deprecated) case BBB
}

struct OptionsAvail1 : OptionSet {
  let rawValue: Int
  static let aaa = OptionsAvail1(rawValue: 1 << 0)
  @available(*, unavailable) static let AAA = OptionsAvail1(rawValue: 1 << 0)
  @available(*, deprecated) static let BBB = OptionsAvail1(rawValue: 1 << 1)
}

func OptionSetTaker1(_ Op : SomeOptions1) {}

func OptionSetTaker2(_ Op : SomeOptions2) {}

func OptionSetTaker3(_ Op1: SomeOptions1, _ Op2: SomeOptions2) {}

func OptionSetTaker4(_ Op1: SomeOptions2, _ Op2: SomeOptions1) {}

func OptionSetTaker5(_ Op1: SomeOptions1, _ Op2: SomeOptions2, _ En1 : SomeEnum1, _ En2: SomeEnum2) {}

func OptionSetTaker6(_ Op1: SomeOptions1, _ Op2: SomeOptions2) {}

func OptionSetTaker6(_ Op1: SomeOptions2, _ Op2: SomeOptions1) {}

func OptionSetTaker7(_ Op1: SomeOptions1, _ Op2: SomeOptions2) -> Int {return 0}

func EnumTaker1(_ E : SomeEnum1) {}
func optionalEnumTaker1(_ : SomeEnum1?) {}

class OptionTakerContainer1 {
  func OptionSetTaker1(_ op : SomeOptions1) {}
  func EnumTaker1(_ E : SomeEnum1) {}
}

class C1 {
  func f1() {
    var f : SomeOptions1
    f = .#^UNRESOLVED_1^#
  }
  func f2() {
    var f : SomeOptions1
    f = [.#^UNRESOLVED_2?check=UNRESOLVED_1^#
  }
  func f3() {
    var f : SomeOptions1
    f = [.Option1, .#^UNRESOLVED_3?check=UNRESOLVED_1^#
  }
}
class C2 {
  func f1() {
    OptionSetTaker1(.#^UNRESOLVED_4?check=UNRESOLVED_1^#)
  }
  func f2() {
    OptionSetTaker1([.Option1, .#^UNRESOLVED_5?check=UNRESOLVED_1^#])
  }
// UNRESOLVED_1-NOT:  SomeEnum1
// UNRESOLVED_1-NOT:  SomeEnum2
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option1[#SomeOptions1#]; name=Option1
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option2[#SomeOptions1#]; name=Option2
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option3[#SomeOptions1#]; name=Option3
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal:        NotOption[#Int#]; name=NotOption
// UNRESOLVED_1-NOT:  NotStaticOption
}

class C3 {
  func f1() {
    OptionSetTaker2(.#^UNRESOLVED_6?check=UNRESOLVED_2^#)
  }
  func f2() {
    OptionSetTaker2([.Option4, .#^UNRESOLVED_7?check=UNRESOLVED_2^#])
  }
// UNRESOLVED_2-NOT:  SomeEnum1
// UNRESOLVED_2-NOT:  SomeEnum2
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option4[#SomeOptions2#]; name=Option4
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option5[#SomeOptions2#]; name=Option5
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option6[#SomeOptions2#]; name=Option6
// UNRESOLVED_2-NOT:  Not
}

class C4 {
  func f1() {
    var E : SomeEnum1
    E = .#^UNRESOLVED_8?check=UNRESOLVED_3^#
  }
  func f2() {
    EnumTaker1(.#^UNRESOLVED_9?check=UNRESOLVED_3^#)
  }
  func f3() {
    OptionSetTaker5(.Option1, .Option4, .#^UNRESOLVED_12?check=UNRESOLVED_3^#, .West)
  }
  func f4() {
    var _: SomeEnum1? = .#^UNRESOLVED_OPT_1?check=UNRESOLVED_3_OPT^#
  }
  func f5() {
    optionalEnumTaker1(.#^UNRESOLVED_OPT_2?check=UNRESOLVED_3_OPT^#)
  }
  func f6() {
    var _: SomeEnum1??? = .#^UNRESOLVED_OPT_3?check=UNRESOLVED_3_OPTOPTOPT^#
  }
}

// Exhaustive to make sure we don't include `SomeOptions1`, `SomeOptions2`, `none` or `some` entries.
// UNRESOLVED_3: Begin completions, 3 items
// UNRESOLVED_3-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
// UNRESOLVED_3-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
// UNRESOLVED_3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#]; name=hash(:)

// Exhaustive to make sure we don't include `init({#(some):` or `init({#nilLiteral:` entries
// UNRESOLVED_3_OPT: Begin completions, 9 items
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#];
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#];
// UNRESOLVED_3_OPT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#];
// UNRESOLVED_3_OPT-DAG: Keyword[nil]/None/Erase[1]/TypeRelation[Convertible]: nil[#SomeEnum1?#]; name=nil
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<SomeEnum1>#]; name=none
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#SomeEnum1#})[#Optional<SomeEnum1>#];
// UNRESOLVED_3_OPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: map({#(self): Optional<SomeEnum1>#})[#((SomeEnum1) throws -> U) -> U?#];
// UNRESOLVED_3_OPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: flatMap({#(self): Optional<SomeEnum1>#})[#((SomeEnum1) throws -> U?) -> U?#];
// UNRESOLVED_3_OPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: hash({#(self): Optional<SomeEnum1>#})[#(into: inout Hasher) -> Void#];

// Exhaustive to make sure we don't include `init({#(some):` or `init({#nilLiteral:` entries
// UNRESOLVED_3_OPTOPTOPT: Begin completions, 9 items
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Keyword[nil]/None/Erase[1]/TypeRelation[Convertible]: nil[#SomeEnum1???#]; name=nil
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<SomeEnum1??>#]; name=none
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#SomeEnum1??#})[#Optional<SomeEnum1??>#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: map({#(self): Optional<SomeEnum1??>#})[#((SomeEnum1??) throws -> U) -> U?#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: flatMap({#(self): Optional<SomeEnum1??>#})[#((SomeEnum1??) throws -> U?) -> U?#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: hash({#(self): Optional<SomeEnum1??>#})[#(into: inout Hasher) -> Void#];

enum Somewhere {
  case earth, mars
}
extension Optional where Wrapped == Somewhere {
  init(str: String) { fatalError() }
  static var nowhere: Self { return nil }
}
func testOptionalWithCustomExtension() {
  var _: Somewhere? = .#^UNRESOLVED_OPT_4^#
// UNRESOLVED_OPT_4: Begin completions, 11 items
// UNRESOLVED_OPT_4-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     earth[#Somewhere#];
// UNRESOLVED_OPT_4-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     mars[#Somewhere#];
// UNRESOLVED_OPT_4-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): Somewhere#})[#(into: inout Hasher) -> Void#];
// UNRESOLVED_OPT_4-DAG: Keyword[nil]/None/Erase[1]/TypeRelation[Convertible]: nil[#Somewhere?#]; name=nil
// UNRESOLVED_OPT_4-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<Somewhere>#]; name=none
// UNRESOLVED_OPT_4-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#Somewhere#})[#Optional<Somewhere>#];
// UNRESOLVED_OPT_4-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init({#str: String#})[#Optional<Somewhere>#]; name=init(str:)
// UNRESOLVED_OPT_4-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: nowhere[#Optional<Somewhere>#]; name=nowhere
// UNRESOLVED_OPT_4-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: map({#(self): Optional<Somewhere>#})[#((Somewhere) throws -> U) -> U?#];
// UNRESOLVED_OPT_4-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: flatMap({#(self): Optional<Somewhere>#})[#((Somewhere) throws -> U?) -> U?#];
// UNRESOLVED_OPT_4-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: hash({#(self): Optional<Somewhere>#})[#(into: inout Hasher) -> Void#];
// UNRESOLVED_OPT_4-NOT: init({#(some):
// UNRESOLVED_OPT_4-NOT: init({#nilLiteral:
}


class C5 {
  func f1() {
    OptionSetTaker3(.Option1, .#^UNRESOLVED_10?check=UNRESOLVED_2^#)
  }
  func f2() {
    OptionSetTaker4(.#^UNRESOLVED_11?check=UNRESOLVED_2^#, .Option1)
  }
}

OptionSetTaker5(.Option1, .Option4, .#^UNRESOLVED_13?check=UNRESOLVED_3^#, .West)
OptionSetTaker5(.#^UNRESOLVED_14?check=UNRESOLVED_1^#, .Option4, .South, .West)
OptionSetTaker5([.#^UNRESOLVED_15?check=UNRESOLVED_1^#], .Option4, .South, .West)

OptionSetTaker6(.#^UNRESOLVED_16^#, .Option4)
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option1[#SomeOptions1#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option2[#SomeOptions1#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option3[#SomeOptions1#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option4[#SomeOptions2#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option5[#SomeOptions2#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option6[#SomeOptions2#];
// UNRESOLVED_16-DAG:  Decl[StaticVar]/CurrNominal:        NotOption[#Int#]; name=NotOption
// UNRESOLVED_16:  End completion


OptionSetTaker6(.Option4, .#^UNRESOLVED_17?check=UNRESOLVED_4^#,)

var a = {() in
  OptionSetTaker5([.#^UNRESOLVED_18?check=UNRESOLVED_1^#], .Option4, .South, .West)
}
var Container = OptionTakerContainer1()
Container.OptionSetTaker1(.#^UNRESOLVED_19?check=UNRESOLVED_1^#)
Container.EnumTaker1(.#^UNRESOLVED_20?check=UNRESOLVED_3^#

func parserSync() {}

// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option1[#SomeOptions1#]; name=Option1
// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option2[#SomeOptions1#]; name=Option2
// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Option3[#SomeOptions1#]; name=Option3
// UNRESOLVED_4-NOT: Option4
// UNRESOLVED_4-NOT: Option5
// UNRESOLVED_4-NOT: Option6

var OpIns1 : SomeOptions1 = .#^UNRESOLVED_21?check=UNRESOLVED_1^#

var c1 = {() -> SomeOptions1 in
  return .#^UNRESOLVED_22?check=UNRESOLVED_1^#
}

var c1_noreturn = {() -> SomeOptions1 in
  .#^UNRESOLVED_22_noreturn?check=UNRESOLVED_1^#
}

class C6 {
  func f1() -> SomeOptions1 {
    return .#^UNRESOLVED_23?check=UNRESOLVED_1^#
  }
  func f2(p : SomeEnum3) {
  switch p {
  case .Payload(.#^UNRESOLVED_24?check=UNRESOLVED_3^#)
  }
  }
}

class C6 {
  func f1(e: SomeEnum1) {
    if let x = Optional(e) where x == .#^UNRESOLVED_25?check=UNRESOLVED_3^#
  }
}
class C7 {}
extension C7 {
  func extendedf1(_ e :SomeEnum1) {}
}

var cInst1 = C7()
cInst1.extendedf1(.#^UNRESOLVED_26?check=UNRESOLVED_3^#

func nocrash1() -> SomeEnum1 {
  return .#^UNRESOLVED_27_NOCRASH?check=NOCRASH^#
}

func resetParser1() {}

func f() -> SomeEnum1 {
  return .#^UNRESOLVED_27?check=UNRESOLVED_3^#
}

let TopLevelVar1 = OptionSetTaker7([.#^UNRESOLVED_28?check=UNRESOLVED_1^#], [.Option4])

let TopLevelVar2 = OptionSetTaker1([.#^UNRESOLVED_29?check=UNRESOLVED_1^#])

let TopLevelVar3 = OptionSetTaker7([.Option1], [.#^UNRESOLVED_30?check=UNRESOLVED_2^#])
let TopLevelVar4 = OptionSetTaker7([.Option1], [.Option4, .#^UNRESOLVED_31?check=UNRESOLVED_2^#])

let _: [SomeEnum1] = [.#^UNRESOLVED_32?check=UNRESOLVED_3^#]
let _: [SomeEnum1] = [.South, .#^UNRESOLVED_33?check=UNRESOLVED_3^#]
let _: [SomeEnum1] = [.South, .#^UNRESOLVED_34?check=UNRESOLVED_3^# .South]

let _: [SomeEnum1:SomeOptions1] = [.South:.Option1, .South:.#^UNRESOLVED_35?check=UNRESOLVED_1^#]
let _: [SomeEnum1:SomeOptions1] = [.South:.Option1, .#^UNRESOLVED_36?check=UNRESOLVED_3^#:.Option1]
let _: [SomeEnum1:SomeOptions1] = [.South:.Option1, .#^UNRESOLVED_37?check=UNRESOLVED_3^#]
let _: [SomeEnum1:SomeOptions1] = [.South:.Option1, .#^UNRESOLVED_38?check=UNRESOLVED_3^#:]
let _: [SomeEnum1:SomeOptions1] = [.#^UNRESOLVED_39?check=UNRESOLVED_3^#]
let _: [SomeEnum1:SomeOptions1] = [.#^UNRESOLVED_40?check=UNRESOLVED_3^#:]
let _: [SomeEnum1:SomeEnum3] = [.South:.Payload(.South), .South: .Payload(.#^UNRESOLVED_41?check=UNRESOLVED_3^#)]
let _: [SomeEnum3:SomeEnum1] = [.Payload(.South):.South, .Payload(.#^UNRESOLVED_42?check=UNRESOLVED_3^#):.South]
let _: [SomeEnum3:SomeEnum1] = [.Payload(.South):.South, .Payload(.#^UNRESOLVED_43?check=UNRESOLVED_3^#):]
let _: [SomeEnum3:SomeEnum1] = [.Payload(.South):.South, .Payload(.#^UNRESOLVED_44?check=UNRESOLVED_3^#)]

func testAvail1(_ x: EnumAvail1) {
  testAvail1(.#^ENUM_AVAIL_1^#)
}
// ENUM_AVAIL_1: Begin completions, 3 items
// ENUM_AVAIL_1-NOT: AAA
// ENUM_AVAIL_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: aaa[#EnumAvail1#];
// ENUM_AVAIL_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/NotRecommended/TypeRelation[Convertible]: BBB[#EnumAvail1#];
// ENUM_AVAIL_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): EnumAvail1#})[#(into: inout Hasher) -> Void#];
// ENUM_AVAIL_1-NOT: AAA

func testAvail2(_ x: OptionsAvail1) {
  testAvail2(.#^OPTIONS_AVAIL_1^#)
}
// OPTIONS_AVAIL_1-NOT: AAA
// OPTIONS_AVAIL_1-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: aaa[#OptionsAvail1#];
// OPTIONS_AVAIL_1-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/NotRecommended/TypeRelation[Convertible]: BBB[#OptionsAvail1#];
// OPTIONS_AVAIL_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init({#rawValue: Int#})[#OptionsAvail1#]
// OPTIONS_AVAIL_1-NOT: AAA

func testWithLiteral1() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Double) {}
  }
  let s: S
  _ = s.takeEnum(thing: .#^WITH_LITERAL_1^#, other: 1.0)
// WITH_LITERAL_1: Begin completions, 2 items
// WITH_LITERAL_1-NEXT: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: myCase[#S.MyEnum#];
// WITH_LITERAL_1-NEXT: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): S.MyEnum#})[#(into: inout Hasher) -> Void#];
}
func testWithLiteral2() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Int) {}
    func takeEnum(thing: MyEnum, other: Double) {}
  }
  let s: S
  _ = s.takeEnum(thing: .#^WITH_LITERAL_2?check=WITH_LITERAL_1^#, other: 1.0)
}
func testWithLiteral3() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Int) {}
    func takeEnum(thing: MyEnum, other: Double) {}
    func test(s: S) {
      _ = s.takeEnum(thing: .#^WITH_LITERAL_3^#, other: 1.0)
// WITH_LITERAL_3: Begin completions, 2 items
// WITH_LITERAL_3-NEXT: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: myCase[#MyEnum#];
// WITH_LITERAL_3-NEXT: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): MyEnum#})[#(into: inout Hasher) -> Void#];
    }
  }
}

func testInvalid1() {
  func invalid() -> NoSuchEnum {
    return .#^INVALID_1?check=NOCRASH^# // Don't crash.
  }
}

func enumFromOtherFile() -> EnumFromOtherFile {
  return .#^OTHER_FILE_1^# // Don't crash.
}
// OTHER_FILE_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: b({#String#})[#EnumFromOtherFile#];
// OTHER_FILE_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: a({#Int#})[#EnumFromOtherFile#];
// OTHER_FILE_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: c[#EnumFromOtherFile#];

struct NonOptSet {
  static let a = NonOptSet()
  static let wrongType = 1
  let notStatic = NonOptSet()
  init(x: Int, y: Int) {}
  init() {}
  static func b() -> NonOptSet { return NonOptSet() }
  static func wrongType() -> Int { return 0 }
  func notStatic() -> NonOptSet { return NonOptSet() }
}

func testNonOptSet() {
  let x: NonOptSet
  x = .#^NON_OPT_SET_1^#
}
// NON_OPT_SET_1: Begin completions, 6 items
// NON_OPT_SET_1-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:    a[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[StaticVar]/CurrNominal:        wrongType[#Int#];
// NON_OPT_SET_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]:  init({#x: Int#}, {#y: Int#})[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]:  init()[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: b()[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[InstanceMethod]/CurrNominal: notStatic({#(self): NonOptSet#})[#() -> NonOptSet#];

func testNonOptSet() {
  let x: NonOptSet = .#^NON_OPT_SET_2?check=NON_OPT_SET_1^#
}

func testNonOptSet() -> NonOptSet {
  return .#^NON_OPT_SET_3?check=NON_OPT_SET_1^#
}

func testInStringInterpolation() {
  enum MyEnum { case foo, bar }
  func takeEnum(_ e: MyEnum) -> MyEnum { return e }
  let x = "enum: \(takeEnum(.#^STRING_INTERPOLATION_1^#))"
  let y = "enum: \(.#^STRING_INTERPOLATION_INVALID?check=NOCRASH^#)" // Dont'crash.
}
// STRING_INTERPOLATION_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: foo[#MyEnum#];
// STRING_INTERPOLATION_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: bar[#MyEnum#];

class BaseClass {
  class SubClass : BaseClass { init() {} }
  static var subInstance: SubClass = SubClass()
  init() {}
  init?(failable: Void) {}
}
protocol MyProtocol {
  typealias Concrete1 = BaseClass
  typealias Concrete2 = AnotherTy
}
extension BaseClass : MyProtocol {}
struct AnotherTy: MyProtocol {}
func testSubType() {
  var _: BaseClass = .#^SUBTYPE_1^#
}
// SUBTYPE_1: Begin completions, 6 items
// SUBTYPE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#BaseClass#];
// SUBTYPE_1-DAG: Decl[Class]/CurrNominal/TypeRelation[Convertible]: SubClass[#BaseClass.SubClass#];
// SUBTYPE_1-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: subInstance[#BaseClass.SubClass#];
// SUBTYPE_1-DAG: Decl[Constructor]/CurrNominal:      init({#failable: Void#})[#BaseClass?#];
// SUBTYPE_1-DAG: Decl[TypeAlias]/Super/TypeRelation[Convertible]: Concrete1[#BaseClass#];
// SUBTYPE_1-DAG: Decl[TypeAlias]/Super:              Concrete2[#AnotherTy#];

func testMemberTypealias() {
  var _: MyProtocol = .#^SUBTYPE_2^#
}
// SUBTYPE_2: Begin completions, 2 items
// SUBTYPE_2-DAG: Decl[TypeAlias]/CurrNominal/TypeRelation[Convertible]: Concrete1[#BaseClass#];
// SUBTYPE_2-DAG: Decl[TypeAlias]/CurrNominal/TypeRelation[Convertible]: Concrete2[#AnotherTy#];

enum Generic<T> {
  case contains(content: T)
  case empty
  static func create(_: T) -> Generic<T> { fatalError() }
}
func takeGenericInt(_: Generic<Int>) { }
func takeGenericU<U>(_: Generic<U>) { }
func testGeneric() {
  do {
    let _: Generic<Int> = .#^GENERIC_1?check=GENERIC_1_INT^#
  }
  takeGenericInt(.#^GENERIC_2?check=GENERIC_1_INT^#)
  takeGenericU(.#^GENERIC_3?check=GENERIC_1_U^#)
}

switch Generic<Int>.empty {
case let .#^GENERIC_4?check=GENERIC_1_INT^#
}
// GENERIC_1_INT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: contains({#content: Int#})[#Generic<Int>#];
// GENERIC_1_INT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: empty[#Generic<Int>#];

// GENERIC_1_U-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: contains({#content: U#})[#Generic<U>#];
// GENERIC_1_U-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: empty[#Generic<U>#];
// GENERIC_1_U-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: create({#U#})[#Generic<U>#];

struct HasCreator {
  static var create: () -> HasCreator = { fatalError() }
  static var create_curried: () -> () -> HasCreator = { fatalError() }
}
func testHasStaticClosure() {
  let _: HasCreator = .#^STATIC_CLOSURE_1^#
}
// STATIC_CLOSURE_1: Begin completions, 3 items
// STATIC_CLOSURE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#HasCreator#];
// FIXME: Suggest 'create()[#HasCreateor#]', not 'create'.
// STATIC_CLOSURE_1-DAG: Decl[StaticVar]/CurrNominal:        create[#() -> HasCreator#];
// STATIC_CLOSURE_1-DAG: Decl[StaticVar]/CurrNominal:        create_curried[#() -> () -> HasCreator#];

struct HasOverloaded {
  init(e: SomeEnum1) {}
  init(e: SomeEnum2) {}
  func takeEnum(_ e: SomeEnum1) -> Int { return 0 }
  func takeEnum(_ e: SomeEnum2) -> Int { return 0 }
}
func testOverload(val: HasOverloaded) {
  let _ = val.takeEnum(.#^OVERLOADED_METHOD_1^#)
// OVERLOADED_METHOD_1: Begin completions, 6 items
// OVERLOADED_METHOD_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
// OVERLOADED_METHOD_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
// OVERLOADED_METHOD_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#];
// OVERLOADED_METHOD_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: East[#SomeEnum2#]; name=East
// OVERLOADED_METHOD_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: West[#SomeEnum2#]; name=West
// OVERLOADED_METHOD_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum2#})[#(into: inout Hasher) -> Void#];

  let _ = HasOverloaded.init(e: .#^OVERLOADED_INIT_1?check=OVERLOADED_METHOD_1^#)
// Same as OVERLOADED_METHOD_1.

  let _ = HasOverloaded(e: .#^OVERLOADED_INIT_2?check=OVERLOADED_METHOD_1^#)
// Same as OVERLOADED_METHOD_1.
}

protocol HasStatic: Equatable {
  static var instance: Self { get }
}
func receiveHasStatic<T: HasStatic>(x: T)  {}
func testingGenericParam1<T: HasStatic>(x: inout T, fn: (T) -> Void) -> T {
  x = .#^GENERICPARAM_1^#
// GENERICPARAM_1: Begin completions, 1 items
// GENERICPARAM_1: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: instance[#HasStatic#]; name=instance

  /* Parser sync. */;

  let _: (Int, T) = (1, .#^GENERICPARAM_2?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.

  (_, x) = (1, .#^GENERICPARAM_3?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.

  let _ = fn(.#^GENERICPARAM_4?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.

  let _ = receiveHasStatic(x: .#^GENERICPARAM_5?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.

  let _ = { () -> T in
    return .#^GENERICPARAM_6?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.
  }
  let _: () -> T = {
    return .#^GENERICPARAM_7?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.
  }
  let _ = { (_: InvalidTy) -> T in
    return .#^GENERICPARAM_8?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.
  }

  if case .#^GENERICPARAM_9?check=GENERICPARAM_1^# = x {}
  // Same as GENERICPARAM_1.

  return .#^GENERICPARAM_10?check=GENERICPARAM_1^#
  // Same as GENERICPARAM_1.
}

class C<T: HasStatic> {

  var t: T = .instance

  func foo(x: T) -> T {
    return .#^GENERICPARAM_11?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.
  }
  func bar<U: HasStatic>(x: U) -> U {
    return .#^GENERICPARAM_12?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.
  }

  func testing() {
    let _ = foo(x: .#^GENERICPARAM_13?check=GENERICPARAM_1^#)
    // Same as GENERICPARAM_1.
    let _ = bar(x: .#^GENERICPARAM_14?check=GENERICPARAM_1^#)
    // Same as GENERICPARAM_1.

    t = .#^GENERICPARAM_15?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.

    /* Parser sync. */; func sync1() {}
    self.t = .#^GENERICPARAM_16?check=GENERICPARAM_1^#
    // Same as GENERICPARAM_1.

    /* Parser sync. */; func sync2() {}
    (_, t) = (1, .#^GENERICPARAM_17?check=GENERICPARAM_1^#)
    // Same as GENERICPARAM_1.

    (_, self.t) = (1, .#^GENERICPARAM_18?check=GENERICPARAM_1^#)
    // Same as GENERICPARAM_1.
  }
}

func testingGenericParam2<X>(obj: C<X>) {
  let _ = obj.foo(x: .#^GENERICPARAM_19?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.
  let _ = obj.bar(x: .#^GENERICPARAM_20?check=GENERICPARAM_1^#)
  // Same as GENERICPARAM_1.
  obj.t = .#^GENERICPARAM_21?check=GENERICPARAM_1^#
  // Same as GENERICPARAM_1.
}

struct TestingStruct {
  var value: SomeEnum1 = .#^DECL_MEMBER_INIT_1?check=UNRESOLVED_3^#
}

func testDefaultArgument(arg: SomeEnum1 = .#^DEFAULT_ARG_1?check=UNRESOLVED_3^#) {}
class TestDefalutArg {
  func method(arg: SomeEnum1 = .#^DEFAULT_ARG_2?check=UNRESOLVED_3^#) {}
  init(arg: SomeEnum1 = .#^DEFAULT_ARG_3?check=UNRESOLVED_3^#) {}
}


struct ConcreteMyProtocol: MyProtocol {}
struct OtherProtocol {}
struct ConcreteOtherProtocol: OtherProtocol {}

struct MyStruct<T> {}
extension MyStruct where T: MyProtocol {
  static var myProtocolOption: MyStruct<ConcreteMyProtocol> { fatalError() }
}
extension MyStruct where T: OtherProtocol {
  static var otherProtocolOption: MyStruct<ConcreteOtherProtocol> { fatalError() }
}

func receiveMyStructOfMyProtocol<T: MyProtocol>(value: MyStruct<T>) {}
func testTypeParamInContextType() {
  receiveMyStructOfMyProtocol(value: .#^TYPEPARAM_IN_CONTEXTTYPE_1^#)
// TYPEPARAM_IN_CONTEXTTYPE_1: Begin completions, 3 items
// TYPEPARAM_IN_CONTEXTTYPE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]:      init()[#MyStruct<MyProtocol>#];
// TYPEPARAM_IN_CONTEXTTYPE_1-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: myProtocolOption[#MyStruct<ConcreteMyProtocol>#];
// TYPEPARAM_IN_CONTEXTTYPE_1-DAG: Decl[StaticVar]/CurrNominal:        otherProtocolOption[#MyStruct<ConcreteOtherProtocol>#];
}

func testTernaryOperator(cond: Bool) {
  let _: SomeEnum1 = cond ? .#^TERNARY_1?check=UNRESOLVED_3^#
  func sync(){}
  let _: SomeEnum1 = cond ? .#^TERNARY_2?check=UNRESOLVED_3^# :
  func sync(){}
  let _: SomeEnum1 = cond ? .#^TERNARY_3?check=UNRESOLVED_3^# : .South
  func sync(){}
  let _: SomeEnum1 = cond ? .South : .#^TERNARY_4?check=UNRESOLVED_3^#
}

func testTernaryOperator2(cond: Bool) {
  let _: SomeEnum1 = cond ? .#^TERNARY_5?check=UNRESOLVED_3^# : .bogus
  func sync(){}
  let _: SomeEnum1 = cond ? .bogus : .#^TERNARY_6?check=UNRESOLVED_3^#
  func sync(){}
  let _: SomeEnum1 = .#^TERNARY_CONDITION^# ? .bogus : .bogus
// TERNARY_CONDITION-DAG: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Convertible]: init()[#Bool#]; name=init()
}

func overloadedClosureRcv(_: () -> SomeEnum1) {}
func overloadedClosureRcv(_: () -> SomeEnum2) {}
func testClosureReturnTypeForOverloaded() {
  overloadedClosureRcv {
    .#^OVERLOADED_CLOSURE_RETURN^#
  }
// OVERLOADED_CLOSURE_RETURN: Begin completions, 6 items
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#];
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#];
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#];
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: East[#SomeEnum2#];
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: West[#SomeEnum2#];
// OVERLOADED_CLOSURE_RETURN-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): SomeEnum2#})[#(into: inout Hasher) -> Void#];
}

func receiveAutoclosure(_: @autoclosure () -> SomeEnum1) {}
func receiveAutoclosureOpt(_: @autoclosure () -> SomeEnum1?) {}
func testAutoclosre() {
  receiveAutoclosure(.#^AUTOCLOSURE?check=UNRESOLVED_3^#)
  // Same as UNRESOLVED_3

  receiveAutoclosureOpt(.#^AUTOCLOSURE_OPT?check=UNRESOLVED_3_OPT^#)
  // Same as UNRESOLVED_3_OPT
}
func testAutoclosreFuncTy(fn: (@autoclosure () -> SomeEnum1) -> Void, fnOpt: (@autoclosure () -> SomeEnum1?) -> Void) {
  fn(.#^AUTOCLOSURE_FUNCTY?check=UNRESOLVED_3^#)
  // Same as UNRESOLVED_3
  fnOpt(.#^AUTOCLOSURE_FUNCTY_OPT?check=UNRESOLVED_3_OPT^#)
  // Same as UNRESOLVED_3_OPT
}

func testSameType() {
  typealias EnumAlias = SomeEnum1
  func testSugarType(_ arg: Optional<SomeEnum1>, arg2: Int8) {}
  func testSugarType(_ arg: SomeEnum1?, arg2: Int16) {}
  func testSugarType(_ arg: Optional<EnumAlias>, arg2: Int32) {}
  func testSugarType(_ arg: EnumAlias?, arg2: Int64) {}
  func testSugarType(_ arg: SomeEnum1, arg2: Int) {}
  func testSugarType(_ arg: EnumAlias, arg2: String) {}

  testSugarType(.#^SUGAR_TYPE^#
// Ensure results aren't duplicated.
// SUGAR_TYPE: Begin completions, 9 items
// SUGAR_TYPE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#];
// SUGAR_TYPE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#];
// SUGAR_TYPE-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#];
// SUGAR_TYPE-DAG: Keyword[nil]/None/Erase[1]/TypeRelation[Convertible]: nil[#SomeEnum1?#];
// SUGAR_TYPE-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<SomeEnum1>#];
// SUGAR_TYPE-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#SomeEnum1#})[#Optional<SomeEnum1>#];
// SUGAR_TYPE-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: map({#(self): Optional<SomeEnum1>#})[#((SomeEnum1) throws -> U) -> U?#];
// SUGAR_TYPE-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: flatMap({#(self): Optional<SomeEnum1>#})[#((SomeEnum1) throws -> U?) -> U?#];
// SUGAR_TYPE-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: hash({#(self): Optional<SomeEnum1>#})[#(into: inout Hasher) -> Void#];
}

struct DispatchTime {
  static func now() -> DispatchTime { .init() }
}
func +(_ x: DispatchTime, _ y: Double) -> DispatchTime { return x }

let _: DispatchTime = .#^UNRESOLVED_FUNCTION_CALL^#now() + 0.2

// UNRESOLVED_FUNCTION_CALL: Begin completions, 2 items
// UNRESOLVED_FUNCTION_CALL-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: now()[#DispatchTime#];
// UNRESOLVED_FUNCTION_CALL-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#DispatchTime#];

func id<T>(_ x: T) -> T { x }

func testNestedExprPatternCompletion(_ x: SomeEnum1) {
  // Multi-statement closures have different type-checking code paths,
  // so we need to test both.
  let fn = {
    switch x {
    case id(.#^UNRESOLVED_NESTED1^#):
      // UNRESOLVED_NESTED1: Begin completions, 3 items
      // UNRESOLVED_NESTED1: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
      // UNRESOLVED_NESTED1: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
      // UNRESOLVED_NESTED1: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#]; name=hash(:)
      break
    }
    if case id(.#^UNRESOLVED_NESTED2^#) = x {}
    // UNRESOLVED_NESTED2: Begin completions, 3 items
    // UNRESOLVED_NESTED2: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
    // UNRESOLVED_NESTED2: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
    // UNRESOLVED_NESTED2: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#]; name=hash(:)
  }
  switch x {
  case id(.#^UNRESOLVED_NESTED3^#):
    // UNRESOLVED_NESTED3: Begin completions, 3 items
    // UNRESOLVED_NESTED3: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
    // UNRESOLVED_NESTED3: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
    // UNRESOLVED_NESTED3: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#]; name=hash(:)
    break
  }
  if case id(.#^UNRESOLVED_NESTED4^#) = x {}
  // UNRESOLVED_NESTED4: Begin completions, 3 items
  // UNRESOLVED_NESTED4: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: South[#SomeEnum1#]; name=South
  // UNRESOLVED_NESTED4: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: North[#SomeEnum1#]; name=North
  // UNRESOLVED_NESTED4: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): SomeEnum1#})[#(into: inout Hasher) -> Void#]; name=hash(:)
}
