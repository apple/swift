// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

func freeFunc() {}

//===---
//===--- Test that we complete calls to constructors (except for 'super'-based calls, which are tested separately).
//===---

struct ImplicitConstructors1 {
}

func testImplicitConstructors1() {
  ImplicitConstructors1#^IMPLICIT_CONSTRUCTORS_1^#
// IMPLICIT_CONSTRUCTORS_1: Begin completions, 3 items
// IMPLICIT_CONSTRUCTORS_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#ImplicitConstructors1#]{{; name=.+$}}
// IMPLICIT_CONSTRUCTORS_1-DAG: Keyword[self]/CurrNominal:     .self[#ImplicitConstructors1.Type#]; name=self
// IMPLICIT_CONSTRUCTORS_1-DAG: Keyword/CurrNominal:           .Type[#ImplicitConstructors1.Type#]; name=Type
}
func testImplicitConstructors1P() {
  ImplicitConstructors1(#^IMPLICIT_CONSTRUCTORS_1P^#
// IMPLICIT_CONSTRUCTORS_1P: Begin completions, 1 items
// IMPLICIT_CONSTRUCTORS_1P: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors1#]; name=
}

struct ImplicitConstructors2 {
  var instanceVar = 0
}

func testImplicitConstructors2() {
  ImplicitConstructors2#^IMPLICIT_CONSTRUCTORS_2^#
// IMPLICIT_CONSTRUCTORS_2: Begin completions, 5 items
// IMPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#ImplicitConstructors2#]{{; name=.+$}}
// IMPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#instanceVar: Int#})[#ImplicitConstructors2#]{{; name=.+$}}
// IMPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#ImplicitConstructors2#]{{; name=.+$}}
// IMPLICIT_CONSTRUCTORS_2-DAG: Keyword[self]/CurrNominal:     .self[#ImplicitConstructors2.Type#]; name=self
// IMPLICIT_CONSTRUCTORS_2-DAG: Keyword/CurrNominal:           .Type[#ImplicitConstructors2.Type#]; name=Type
}
func testImplicitConstructors2P() {
  ImplicitConstructors2(#^IMPLICIT_CONSTRUCTORS_2P^#
// IMPLICIT_CONSTRUCTORS_2P: Begin completions, 3 items
// IMPLICIT_CONSTRUCTORS_2P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors2#]; name=
// IMPLICIT_CONSTRUCTORS_2P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#instanceVar: Int#}[')'][#ImplicitConstructors2#]{{; name=.+$}}
// IMPLICIT_CONSTRUCTORS_2P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors2#]; name=
}

struct ExplicitConstructors1 {
  init() {}
  init(a: Int) {}
  init(a: Int, b: Float) {}
}

func testExplicitConstructors1() {
  ExplicitConstructors1#^EXPLICIT_CONSTRUCTORS_1^#
// EXPLICIT_CONSTRUCTORS_1: Begin completions, 5 items
// EXPLICIT_CONSTRUCTORS_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#ExplicitConstructors1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#a: Int#})[#ExplicitConstructors1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#a: Int#}, {#b: Float#})[#ExplicitConstructors1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_1-DAG: Keyword[self]/CurrNominal: .self[#ExplicitConstructors1.Type#]; name=self
// EXPLICIT_CONSTRUCTORS_1-DAG: Keyword/CurrNominal:       .Type[#ExplicitConstructors1.Type#]; name=Type
}
func testExplicitConstructors1P() {
  ExplicitConstructors1(#^EXPLICIT_CONSTRUCTORS_1P^#
// EXPLICIT_CONSTRUCTORS_1P: Begin completions, 3 items
// EXPLICIT_CONSTRUCTORS_1P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ExplicitConstructors1#]; name=
// EXPLICIT_CONSTRUCTORS_1P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}[')'][#ExplicitConstructors1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_1P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}, {#b: Float#}[')'][#ExplicitConstructors1#]{{; name=.+$}}
}

ExplicitConstructors1#^EXPLICIT_CONSTRUCTORS_2^#

// EXPLICIT_CONSTRUCTORS_2: Begin completions, 5 items
// EXPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#ExplicitConstructors1#]
// EXPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#a: Int#})[#ExplicitConstructors1#]
// EXPLICIT_CONSTRUCTORS_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#a: Int#}, {#b: Float#})[#ExplicitConstructors1#]
// EXPLICIT_CONSTRUCTORS_2-DAG: Keyword[self]/CurrNominal:     .self[#ExplicitConstructors1.Type#]; name=self
// EXPLICIT_CONSTRUCTORS_2-DAG: Keyword/CurrNominal:           .Type[#ExplicitConstructors1.Type#]; name=Type

ExplicitConstructors1(#^EXPLICIT_CONSTRUCTORS_2P^#

// EXPLICIT_CONSTRUCTORS_2P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}[')'][#ExplicitConstructors1#]
// EXPLICIT_CONSTRUCTORS_2P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}, {#b: Float#}[')'][#ExplicitConstructors1#]


struct ExplicitConstructors3 {
  init() {}
  init(_ a: Int) {}
  init(a: Int, b: Float) {}
}

func testExplicitConstructors3P() {
  ExplicitConstructors3(#^EXPLICIT_CONSTRUCTORS_3P^#
// EXPLICIT_CONSTRUCTORS_3P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): Int#}[')'][#ExplicitConstructors3#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_3P-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}, {#b: Float#}[')'][#ExplicitConstructors3#]{{; name=.+$}}
}


struct ExplicitConstructorsSelector1 {
  init(int a : Int) {}
  init(int a : Int, andFloat b : Float) {}
}

func testExplicitConstructorsSelector1() {
  ExplicitConstructorsSelector1#^EXPLICIT_CONSTRUCTORS_SELECTOR_1^#
// EXPLICIT_CONSTRUCTORS_SELECTOR_1: Begin completions, 4 items
// EXPLICIT_CONSTRUCTORS_SELECTOR_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#int: Int#})[#ExplicitConstructorsSelector1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#int: Int#}, {#andFloat: Float#})[#ExplicitConstructorsSelector1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_1-DAG: Keyword[self]/CurrNominal:     .self[#ExplicitConstructorsSelector1.Type#]; name=self
// EXPLICIT_CONSTRUCTORS_SELECTOR_1-DAG: Keyword/CurrNominal:           .Type[#ExplicitConstructorsSelector1.Type#]; name=Type
}

func testExplicitConstructorsValueMetatype() {
  var SS = ExplicitConstructorsSelector1.self
  SS#^EXPLICIT_CONSTRUCTORS_VAL_META_1^#
  SS(#^EXPLICIT_CONSTRUCTORS_VAL_META_2^#

// EXPLICIT_CONSTRUCTORS_VAL_META_1: Decl[Constructor]/CurrNominal: .init({#int: Int#})[#ExplicitConstructorsSelector1#]
// EXPLICIT_CONSTRUCTORS_VAL_META_1: Decl[Constructor]/CurrNominal: .init({#int: Int#}, {#andFloat: Float#})[#ExplicitConstructorsSelector1#]

// EXPLICIT_CONSTRUCTORS_VAL_META_2-NOT: Decl[Constructor]
}

struct ExplicitConstructorsSelector2 {
  init(noArgs _ : ()) {}
  init(_ a : Int) {}
  init(_ a : Int, withFloat b : Float) {}
  init(int a : Int, _ b : Float) {}
}

func testExplicitConstructorsSelector2() {
  ExplicitConstructorsSelector2#^EXPLICIT_CONSTRUCTORS_SELECTOR_2^#
// EXPLICIT_CONSTRUCTORS_SELECTOR_2: Begin completions, 6 items
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#noArgs: ()#})[#ExplicitConstructorsSelector2#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#(a): Int#})[#ExplicitConstructorsSelector2#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#(a): Int#}, {#withFloat: Float#})[#ExplicitConstructorsSelector2#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#int: Int#}, {#(b): Float#})[#ExplicitConstructorsSelector2#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Keyword[self]/CurrNominal:     .self[#ExplicitConstructorsSelector2.Type#]; name=self
// EXPLICIT_CONSTRUCTORS_SELECTOR_2-DAG: Keyword/CurrNominal:           .Type[#ExplicitConstructorsSelector2.Type#]; name=Type
}

class ExplicitConstructorsBase1 {
  init() {}
  init(a : Int) {}
}

class ExplicitConstructorsDerived1 : ExplicitConstructorsBase1 {
  init() {}
  init(a : Int) {}
}

class ExplicitConstructorsDerived2 : ExplicitConstructorsBase1 {
  init() {}
  required init(a : Int) {}
  class func foo() {
    self.#^INIT_FROM_METATYPE5?check=INIT_FROM_METATYPE4^#
  }
}

func testExplicitConstructorsBaseDerived1() {
  ExplicitConstructorsDerived1#^EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1^#
}
// EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1: Begin completions, 4 items
// EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:  ()[#ExplicitConstructorsDerived1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:  ({#a: Int#})[#ExplicitConstructorsDerived1#]{{; name=.+$}}
// EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1-DAG: Keyword[self]/CurrNominal:     .self[#ExplicitConstructorsDerived1.Type#]; name=self
// EXPLICIT_CONSTRUCTORS_BASE_DERIVED_1-DAG: Keyword/CurrNominal:           .Type[#ExplicitConstructorsDerived1.Type#]; name=Type

func testGetInitFromMetatype1() {
  ExplicitConstructorsBase1.#^INIT_FROM_METATYPE1^#
}

// INIT_FROM_METATYPE1: Begin completions, 4 items
// INIT_FROM_METATYPE1-DAG: Keyword[self]/CurrNominal:          self[#ExplicitConstructorsBase1.Type#]; name=self
// INIT_FROM_METATYPE1-DAG: Keyword/CurrNominal:                Type[#ExplicitConstructorsBase1.Type#]; name=Type
// INIT_FROM_METATYPE1-DAG: Decl[Constructor]/CurrNominal:      init()[#ExplicitConstructorsBase1#]{{; name=.+$}}
// INIT_FROM_METATYPE1-DAG: Decl[Constructor]/CurrNominal:      init({#a: Int#})[#ExplicitConstructorsBase1#]{{; name=.+$}}

func testGetInitFromMetatype2() {
  var S1 = ExplicitConstructorsBase1.self
  var S2 = ExplicitConstructorsDerived2.self
  S1.#^INIT_FROM_METATYPE2_1?check=INIT_FROM_METATYPE2_NOINIT^#
  S1#^INIT_FROM_METATYPE2_2?check=INIT_FROM_METATYPE2_NOINIT^#
  S1(#^INIT_FROM_METATYPE2_3?check=INIT_FROM_METATYPE2_NOINIT^#
  S2.#^INIT_FROM_METATYPE2_4?check=INIT_FROM_METATYPE2_SHOW_INIT^#
  S2#^INIT_FROM_METATYPE2_5?check=INIT_FROM_METATYPE2_SHOW_INIT^#
  S2(#^INIT_FROM_METATYPE2_6?check=INIT_FROM_METATYPE2_NOINIT^#
}
// INIT_FROM_METATYPE2_NOINIT-NOT: Decl[Constructor]

// INIT_FROM_METATYPE2_SHOW_INIT-NOT: Decl[Constructor]
// INIT_FROM_METATYPE2_SHOW_INIT: Decl[Constructor]/CurrNominal: {{init|.init}}({#a: Int#})[#ExplicitConstructorsDerived2#]
// INIT_FROM_METATYPE2_SHOW_INIT-NOT: Decl[Constructor]

func testGetInitFromMetatype3() {
  var SS = ExplicitConstructorsBase1.self
  type(of: SS).#^INIT_FROM_METATYPE3^#
}

// INIT_FROM_METATYPE3-NOT: Decl[Constructor]/CurrNominal:      init()[#ExplicitConstructorsBase1#]{{; name=.+$}}

func testGetInitFromMetatype4() {
  var a = ExplicitConstructorsDerived2()
  type(of: a).#^INIT_FROM_METATYPE4^#
}

// INIT_FROM_METATYPE4: Decl[Constructor]/CurrNominal: init({#a: Int#})[#ExplicitConstructorsDerived2#]{{; name=.+$}}
// INIT_FROM_METATYPE4-NOT: Decl[Constructor]/CurrNominal:      init()[#ExplicitConstructorsDerived2#]{{; name=.+$}}

struct ExplicitConstructorsDerived3 {
  init() {}
  required init(a : Int) {}
  static func foo() {
    self.#^INIT_FROM_METATYPE6^#
  }
// INIT_FROM_METATYPE6: Decl[Constructor]/CurrNominal:      init()[#ExplicitConstructorsDerived3#]{{; name=.+$}}
// INIT_FROM_METATYPE6: Decl[Constructor]/CurrNominal:      init({#a: Int#})[#ExplicitConstructorsDerived3#]{{; name=.+$}}
}

func testHaveRParen1() {
  ImplicitConstructors1(#^HAVE_RPAREN_1^#)
// HAVE_RPAREN_1: Begin completions, 1 items
// HAVE_RPAREN_1: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors1#]; name=
}

func testHaveRParen2() {
  ImplicitConstructors2(#^HAVE_RPAREN_2^#)
// HAVE_RPAREN_2: Begin completions, 3 items
// HAVE_RPAREN_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors2#]; name=
// HAVE_RPAREN_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#instanceVar: Int#}[')'][#ImplicitConstructors2#]{{; name=.+$}}
// HAVE_RPAREN_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#ImplicitConstructors2#]; name=
}

func testHaveComma1() {
  ExplicitConstructors1(#^HAVE_COMMA_1?check=EXPLICIT_CONSTRUCTORS_1P^#,
}

//===--- Test that we show default constuctors inherited from protocols

protocol ProtDefaultInit {}
extension ProtDefaultInit { init(foo: Int) {}}

class ConformsToProtDefaultInit: ProtDefaultInit {
  init(bar: Int) {}
}

func testHasDefaultInitFromProtocol() {
  ConformsToProtDefaultInit#^DEFAULT_INIT_FROM_PROT_NODOT?check=DEFAULT_INIT_FROM_PROT^#
  ConformsToProtDefaultInit.#^DEFAULT_INIT_FROM_PROT_DOT?check=DEFAULT_INIT_FROM_PROT^#
  ConformsToProtDefaultInit(#^DEFAULT_INIT_FROM_PROT_PAREN?check=DEFAULT_INIT_FROM_PROT^#

// DEFAULT_INIT_FROM_PROT-DAG: Decl[Constructor]/CurrNominal{{.*}}:  {{.+}}{#bar: Int#}
// DEFAULT_INIT_FROM_PROT-DAG: Decl[Constructor]/Super{{.*}}:        {{.+}}{#foo: Int#}
}

class WithAlias1 {
  init(busted: B) {}
  init(working: Int) {}
}
typealias Alias1 = WithAlias1
func testWithAlias1() {
  Alias1#^WITH_ALIAS_1^#
}
// WITH_ALIAS_1: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#working: Int#})[#Alias1#];

struct ClosureInInit1 {
  struct S {init(_: Int) {}}
  var prop1: S = {
    return S(#^CLOSURE_IN_INIT_1^#
  }
// CLOSURE_IN_INIT_1: Decl[Constructor]/CurrNominal/Flair[ArgLabels]{{(/TypeRelation\[Convertible\])?}}:      ['(']{#Int#}[')'][#S#];
  var prop2: S = {
    return S(#^CLOSURE_IN_INIT_2?check=CLOSURE_IN_INIT_1^#
  }()
  var prop3: S = {
    S(#^CLOSURE_IN_INIT_3?check=CLOSURE_IN_INIT_1^#
  }
  var prop3: S = {
    S(#^CLOSURE_IN_INIT_4?check=CLOSURE_IN_INIT_1^#
  }()
}

public class AvailableTest {

  @available(swift, obsoleted: 4)
  init(opt: Int) { }

  @available(swift, introduced: 4)
  init?(opt: Int) { }

  init(normal1: Int) { }
  init(normal2: Int) { }


}
func testAvailable() {
  let _ = AvailableTest(#^AVAILABLE_1^#
// AVAILABLE_1: Begin completions, 3 items
// AVAILABLE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#opt: Int#}[')'][#AvailableTest?#]; name=opt:
// AVAILABLE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#normal1: Int#}[')'][#AvailableTest#]; name=normal1:
// AVAILABLE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#normal2: Int#}[')'][#AvailableTest#]; name=normal2:

  let _ = AvailableTest.init(#^AVAILABLE_2?check=AVAILABLE_1^#
}

protocol DataType {
  associatedtype Content
}
class DependentTypeInClosure<Data: DataType> {
  init(_ arg: Data, fn: (Data.Content) -> Void) {}
  init(arg: Data, fn: () -> Data.Content) {}
}
func testDependentTypeInClosure() {
  let _: DependentTypeInClosure = .#^DEPENDENT_IN_CLOSURE_3^#
// DEPENDENT_IN_CLOSURE_3: Begin completions, 2 items
// DEPENDENT_IN_CLOSURE_3-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init({#(arg): DataType#}, {#fn: (Data.Content) -> Void##(Data.Content) -> Void#})[#DependentTypeInClosure<DataType>#];
// DEPENDENT_IN_CLOSURE_3-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init({#arg: DataType#}, {#fn: () -> Data.Content##() -> Data.Content#})[#DependentTypeInClosure<DataType>#];

  let _ = DependentTypeInClosure(#^DEPENDENT_IN_CLOSURE_1^#)
// DEPENDENT_IN_CLOSURE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#(arg): DataType#}, {#fn: (_) -> Void##(_) -> Void#}[')'][#DependentTypeInClosure<DataType>#];
// DEPENDENT_IN_CLOSURE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#arg: DataType#}, {#fn: () -> _##() -> _#}[')'][#DependentTypeInClosure<DataType>#];

  let _ = DependentTypeInClosure.#^DEPENDENT_IN_CLOSURE_2^#
// DEPENDENT_IN_CLOSURE_2: Begin completions, 4 items
// DEPENDENT_IN_CLOSURE_2-DAG: Keyword[self]/CurrNominal:          self[#DependentTypeInClosure<_>.Type#]; name=self
// DEPENDENT_IN_CLOSURE_2-DAG: Keyword/CurrNominal:                Type[#DependentTypeInClosure<_>.Type#]; name=Type
// DEPENDENT_IN_CLOSURE_2-DAG: Decl[Constructor]/CurrNominal:      init({#(arg): _#}, {#fn: (_.Content) -> Void##(_.Content) -> Void#})[#DependentTypeInClosure<_>#]; name=init(:fn:)
// DEPENDENT_IN_CLOSURE_2-DAG: Decl[Constructor]/CurrNominal:      init({#arg: _#}, {#fn: () -> _.Content##() -> _.Content#})[#DependentTypeInClosure<_>#]; name=init(arg:fn:)
}
struct InitWithUnresolved<Data: DataType> where Data.Content: Comparable {
  init(arg: Data, fn: (Data.Content) -> Void) {}
}
extension InitWithUnresolved where Self.Data: Comparable {
  init(arg2: Data) {}
}
func testInitWithUnresolved() {
  let _ = InitWithUnresolved(#^INIT_WITH_UNRESOLVEDTYPE_1^#
// INIT_WITH_UNRESOLVEDTYPE_1: Begin completions, 2 items
// INIT_WITH_UNRESOLVEDTYPE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#arg: DataType#}, {#fn: (_) -> Void##(_) -> Void#}[')'][#InitWithUnresolved<DataType>#];
// INIT_WITH_UNRESOLVEDTYPE_1-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#arg2: DataType#}[')'][#InitWithUnresolved<DataType>#];
}

func testIgnoreGenericArgsAfterCompletionToken() {
  struct IPv4 {}

  public struct HostRecord<IPType> {
    init(position: inout Int) throws {}
  }

  func deserializeRecord() throws -> HostRecord<IPv4> {
    var position = 42
    return try #^IGNORE_GENERIC_ARGS_AFTER_COMPLETION_TOKEN^#HostRecord<IPv4>(position: &position)
// IGNORE_GENERIC_ARGS_AFTER_COMPLETION_TOKEN-DAG: Decl[Struct]/Local: HostRecord[#HostRecord#];
  }
}
