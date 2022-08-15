// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

//===---
//===--- Test that we can complete enum elements.
//===---

//===--- Helper types.

enum FooEnum: CaseIterable {
  case Foo1
  case Foo2
  static var alias1: FooEnum { return .Foo1 }
}

// FOO_ENUM_TYPE_CONTEXT: Begin completions
// FOO_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Foo1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Foo2[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_TYPE_CONTEXT-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: .alias1[#FooEnum#]; name=alias1
// FOO_ENUM_TYPE_CONTEXT: End completions

// FOO_ENUM_NO_DOT: Begin completions
// FOO_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal: .Foo1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal: .Foo2[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal: .alias1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .hash({#(self): FooEnum#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[TypeAlias]/CurrNominal: .AllCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal: .allCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_NO_DOT-NEXT: Keyword[self]/CurrNominal:   .self[#FooEnum.Type#]; name=self
// FOO_ENUM_NO_DOT-NEXT: Keyword/CurrNominal: .Type[#FooEnum.Type#]; name=Type
// FOO_ENUM_NO_DOT-NEXT: End completions

// FOO_ENUM_DOT: Begin completions
// FOO_ENUM_DOT-NEXT: Keyword[self]/CurrNominal: self[#FooEnum.Type#]; name=self
// FOO_ENUM_DOT-NEXT: Keyword/CurrNominal: Type[#FooEnum.Type#]; name=Type
// FOO_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal: Foo1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal: Foo2[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal: alias1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: hash({#(self): FooEnum#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: Decl[TypeAlias]/CurrNominal: AllCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal: allCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_DOT-NEXT: End completions

// FOO_ENUM_DOT_CONTEXT: Begin completions
// FOO_ENUM_DOT_CONTEXT-NEXT: Keyword[self]/CurrNominal: self[#FooEnum.Type#]; name=self
// FOO_ENUM_DOT_CONTEXT-NEXT: Keyword/CurrNominal: Type[#FooEnum.Type#]; name=Type
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: Foo1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: Foo2[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: alias1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): FooEnum#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[TypeAlias]/CurrNominal: AllCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: Decl[StaticVar]/CurrNominal: allCases[#[FooEnum]#]{{; name=.+$}}
// FOO_ENUM_DOT_CONTEXT-NEXT: End completions

// FOO_ENUM_DOT_ELEMENTS: Begin completions, 6 items
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Foo1[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: Foo2[#FooEnum#]{{; name=.+$}}
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: alias1[#FooEnum#]; name=alias1
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): FooEnum#})[#(into: inout Hasher) -> Void#]; name=hash(:)
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[TypeAlias]/CurrNominal: AllCases[#[FooEnum]#]; name=AllCases
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[StaticVar]/CurrNominal:        allCases[#[FooEnum]#]; name=allCases
// FOO_ENUM_DOT_ELEMENTS-NEXT: End completions

enum BarEnum {
  case Bar1
  case Bar2()
  case Bar3(Int)
  case Bar4(a: Int, b: Float)
  case Bar5(a: Int, (Float))
  case Bar6(a: Int, b: (Float))
  case Bar7(a: Int, (b: Float, c: Double))
  case Bar8(a: Int, b: (c: Float, d: Double))
  case Bar9(Int)
  case Bar10(Int, Float)
  case Bar11(Int, (Float))
  case Bar12(Int, (Float, Double))

  mutating
  func barInstanceFunc() {}
  static var staticVar: Int = 12
  static func barStaticFunc() {}
}

// BAR_ENUM_TYPE_CONTEXT: Begin completions
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar1[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar2()[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar3({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar4({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar5({#a: Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar6({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar7({#a: Int#}, {#(b: Float, c: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar8({#a: Int#}, {#b: (c: Float, d: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar9({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar10({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar11({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Bar12({#Int#}, {#(Float, Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_TYPE_CONTEXT: End completions

// BAR_ENUM_NO_DOT: Begin completions
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar1[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar2()[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar3({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar4({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar5({#a: Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar6({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar7({#a: Int#}, {#(b: Float, c: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar8({#a: Int#}, {#b: (c: Float, d: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar9({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar10({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar11({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:      .Bar12({#Int#}, {#(Float, Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal:   .barInstanceFunc({#(self): &BarEnum#})[#() -> Void#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:        .staticVar[#Int#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:     .barStaticFunc()[#Void#]{{; name=.+$}}
// BAR_ENUM_NO_DOT-NEXT: Keyword[self]/CurrNominal:          .self[#BarEnum.Type#]; name=self
// BAR_ENUM_NO_DOT-NEXT: Keyword/CurrNominal:                .Type[#BarEnum.Type#]; name=Type
// BAR_ENUM_NO_DOT-NEXT: End completions

// BAR_ENUM_DOT: Begin completions
// BAR_ENUM_DOT-NEXT: Keyword[self]/CurrNominal:          self[#BarEnum.Type#]; name=self
// BAR_ENUM_DOT-NEXT: Keyword/CurrNominal:                Type[#BarEnum.Type#]; name=Type
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar1[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar2()[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar3({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar4({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar5({#a: Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar6({#a: Int#}, {#b: Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar7({#a: Int#}, {#(b: Float, c: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar8({#a: Int#}, {#b: (c: Float, d: Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar9({#Int#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar10({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar11({#Int#}, {#Float#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:      Bar12({#Int#}, {#(Float, Double)#})[#BarEnum#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal:   barInstanceFunc({#(self): &BarEnum#})[#() -> Void#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:        staticVar[#Int#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal/TypeRelation[Invalid]: barStaticFunc()[#Void#]{{; name=.+$}}
// BAR_ENUM_DOT-NEXT: End completions

enum BazEnum<T> {
  case Baz1
  case Baz2(T)

  mutating
  func bazInstanceFunc() {}
  static var staticVar: Int = 12
  static var staticVarT: T = 17
  static func bazStaticFunc() {}
}

// BAZ_ENUM_TYPE_CONTEXT: Begin completions
// BAZ_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Baz1[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Baz2({#Int#})[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_ENUM_TYPE_CONTEXT: End completions

// BAZ_INT_ENUM_NO_DOT: Begin completions, 8 items
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz1[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz2({#Int#})[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .bazInstanceFunc({#(self): &BazEnum<Int>#})[#() -> Void#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVarT[#Int#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .bazStaticFunc()[#Void#]{{; name=.+$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Keyword[self]/CurrNominal:        .self[#BazEnum<Int>.Type#]; name=self
// BAZ_INT_ENUM_NO_DOT-NEXT: Keyword/CurrNominal:              .Type[#BazEnum<Int>.Type#]; name=Type
// BAZ_INT_ENUM_NO_DOT-NEXT: End completions

// BAZ_T_ENUM_NO_DOT: Begin completions
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz1[#BazEnum<_>#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz2({#_#})[#BazEnum<_>#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .bazInstanceFunc({#(self): &BazEnum<_>#})[#() -> Void#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVarT[#_#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .bazStaticFunc()[#Void#]{{; name=.+$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Keyword[self]/CurrNominal:        .self[#BazEnum<_>.Type#]; name=self
// BAZ_T_ENUM_NO_DOT-NEXT: Keyword/CurrNominal:              .Type[#BazEnum<_>.Type#]; name=Type
// BAZ_T_ENUM_NO_DOT-NEXT: End completions

// BAZ_INT_ENUM_DOT: Begin completions, 8 items
// BAZ_INT_ENUM_DOT-NEXT: Keyword[self]/CurrNominal:        self[#BazEnum<Int>.Type#]; name=self
// BAZ_INT_ENUM_DOT-NEXT: Keyword/CurrNominal:              Type[#BazEnum<Int>.Type#]; name=Type
// BAZ_INT_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz1[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz2({#Int#})[#BazEnum<Int>#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc({#(self): &BazEnum<Int>#})[#() -> Void#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVarT[#Int#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal/TypeRelation[Invalid]:   bazStaticFunc()[#Void#]{{; name=.+$}}
// BAZ_INT_ENUM_DOT-NEXT: End completions

// BAZ_T_ENUM_DOT: Begin completions, 8 items
// BAZ_T_ENUM_DOT-NEXT: Keyword[self]/CurrNominal:        self[#BazEnum<_>.Type#]; name=self
// BAZ_T_ENUM_DOT-NEXT: Keyword/CurrNominal:              Type[#BazEnum<_>.Type#]; name=Type
// BAZ_T_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz1[#BazEnum<_>#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz2({#_#})[#BazEnum<_>#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc({#(self): &BazEnum<_>#})[#() -> Void#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVarT[#_#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal/TypeRelation[Invalid]:   bazStaticFunc()[#Void#]{{; name=.+$}}
// BAZ_T_ENUM_DOT-NEXT: End completions

enum QuxEnum : Int {
  case Qux1 = 10
  case Qux2 = 20
}

// QUX_ENUM_TYPE_CONTEXT: Begin completions
// QUX_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Qux1[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .Qux2[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_TYPE_CONTEXT: End completions

// QUX_ENUM_NO_DOT: Begin completions, 7 items
// QUX_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Qux1[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Qux2[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      .RawValue[#Int#]{{; name=.+$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#rawValue: Int#})[#QuxEnum?#]{{; name=.+$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/Super/IsSystem: .hash({#(self): QuxEnum#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// QUX_ENUM_NO_DOT-NEXT: Keyword[self]/CurrNominal:        .self[#QuxEnum.Type#]; name=self
// QUX_ENUM_NO_DOT-NEXT: Keyword/CurrNominal:              .Type[#QuxEnum.Type#]; name=Type
// QUX_ENUM_NO_DOT-NEXT: End completions

// QUX_ENUM_DOT: Begin completions, 7 items
// QUX_ENUM_DOT-NEXT: Keyword[self]/CurrNominal:        self[#QuxEnum.Type#]; name=self
// QUX_ENUM_DOT-NEXT: Keyword/CurrNominal:              Type[#QuxEnum.Type#]; name=Type
// QUX_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Qux1[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Qux2[#QuxEnum#]{{; name=.+$}}
// QUX_ENUM_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      RawValue[#Int#]{{; name=.+$}}
// QUX_ENUM_DOT-NEXT: Decl[Constructor]/CurrNominal:    init({#rawValue: Int#})[#QuxEnum?#]{{; name=.+$}}
// QUX_ENUM_DOT-NEXT: Decl[InstanceMethod]/Super/IsSystem: hash({#(self): QuxEnum#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// QUX_ENUM_DOT-NEXT: End completions

func freeFunc() {}

// WITH_GLOBAL_RESULTS: Begin completions
// WITH_GLOBAL_RESULTS: Decl[FreeFunction]/CurrModule/TypeRelation[Invalid]: freeFunc()[#Void#]{{; name=.+$}}
// WITH_GLOBAL_RESULTS: End completions

//===--- Complete enum elements in 'switch'.

func testSwitch1(e: FooEnum) {
  switch e {
  case #^ENUM_SW_1?check=WITH_GLOBAL_RESULTS;check=FOO_ENUM_TYPE_CONTEXT^#
  }
}
func testSwitch2(e: FooEnum) {
  switch e {
  case .Foo1:
  case #^ENUM_SW_2?check=WITH_GLOBAL_RESULTS;check=FOO_ENUM_TYPE_CONTEXT^#
  }
}
func testSwitch3(e: BarEnum) {
  switch e {
  case #^ENUM_SW_3?check=WITH_GLOBAL_RESULTS;check=BAR_ENUM_TYPE_CONTEXT^#
  }
}
func testSwitch4(e: BazEnum<Int>) {
  switch e {
  case #^ENUM_SW_4?check=WITH_GLOBAL_RESULTS;check=BAZ_ENUM_TYPE_CONTEXT^#
  }
}
func testSwitch5(e: QuxEnum) {
  switch e {
  case #^ENUM_SW_5?check=WITH_GLOBAL_RESULTS;check=QUX_ENUM_TYPE_CONTEXT^#
  }
}

// Test for top level code
switch QuxEnum.Qux1 {
case #^ENUM_SW_6?check=WITH_GLOBAL_RESULTS;check=QUX_ENUM_TYPE_CONTEXT^#
}

func testSwitchWithDot1(e: FooEnum) {
  switch e {
  case .#^ENUM_SW_WITH_DOT_1?check=FOO_ENUM_DOT_ELEMENTS^#
  }
}

// Test for top level code
switch FooEnum.Foo2 {
case .#^ENUM_SW_WITH_DOT_2?check=FOO_ENUM_DOT_ELEMENTS^#
}

func testSwitchWithQualification1(e: FooEnum) {
  switch e {
  case FooEnum.#^ENUM_SW_WITH_QUAL_1?check=FOO_ENUM_DOT_CONTEXT^#
  }
}

func testSwitchExprError1() {
  switch unknown_var {
  case FooEnum.#^ENUM_SW_EXPR_ERROR_1?check=FOO_ENUM_DOT^#
  }
}

// FIXME
func testSwitchInPattern1(e: BazEnum<Int>) {
  switch e {
  case .Baz2(#^ENUM_SW_IN_PATTERN_1?check=WITH_GLOBAL_RESULTS;check=ENUM_SW_IN_PATTERN_1^#
  }
}
// ENUM_SW_IN_PATTERN_1-NOT: .Baz1

func testSwitchInPattern2(e: BazEnum<Int>) {
  switch e {
  case .Baz2(.#^ENUM_SW_IN_PATTERN_2^#
  }
}
// ENUM_SW_IN_PATTERN_2-NOT: .Baz1

//===--- Complete qualified references to enum elements.

func testQualifiedNoDot1() {
  var e = FooEnum#^ENUM_QUAL_NO_DOT_1?check=FOO_ENUM_NO_DOT^#
}
func testQualifiedNoDot2() {
  var e = BarEnum#^ENUM_QUAL_NO_DOT_2?check=BAR_ENUM_NO_DOT^#
}
func testQualifiedNoDot3() {
  var e = BazEnum<Int>#^ENUM_QUAL_NO_DOT_3?check=BAZ_INT_ENUM_NO_DOT^#
}
func testQualifiedNoDot4() {
  var e = BazEnum#^ENUM_QUAL_NO_DOT_4?check=BAZ_T_ENUM_NO_DOT^#
}
func testQualifiedNoDot5() {
  var e = QuxEnum#^ENUM_QUAL_NO_DOT_5?check=QUX_ENUM_NO_DOT^#
}

func testQualifiedDot1() {
  var e = FooEnum.#^ENUM_QUAL_DOT_1?check=FOO_ENUM_DOT^#
}
func testQualifiedDot2() {
  var e = BarEnum.#^ENUM_QUAL_DOT_2?check=BAR_ENUM_DOT^#
}
func testQualifiedDot3() {
  var e = BazEnum<Int>.#^ENUM_QUAL_DOT_3?check=BAZ_INT_ENUM_DOT^#
}
func testQualifiedDot4() {
  var e = BazEnum.#^ENUM_QUAL_DOT_4?check=BAZ_T_ENUM_DOT^#
}
func testQualifiedDot5() {
  var e = QuxEnum.#^ENUM_QUAL_DOT_5?check=QUX_ENUM_DOT^#
}

// ===--- Complete in the presence of invalid enum elements.

enum WithInvalid {
  case Okay
  case NotOkay.
  case .AlsoNotOkay
  case
  case JustFine
}

func testWithInvalid1() {
  let x = WithInvalid.#^WITH_INVALID_DOT_1?check=WITH_INVALID_DOT^#

// WITH_INVALID_DOT: Begin completions
// WITH_INVALID_DOT-DAG: Decl[EnumElement]/CurrNominal:      Okay[#WithInvalid#]; name=Okay
// WITH_INVALID_DOT-DAG: Decl[EnumElement]/CurrNominal:      NotOkay[#WithInvalid#]; name=NotOkay
// WITH_INVALID_DOT-DAG: Decl[EnumElement]/CurrNominal:      AlsoNotOkay[#WithInvalid#]; name=AlsoNotOkay
// WITH_INVALID_DOT-DAG: Decl[EnumElement]/CurrNominal:      JustFine[#WithInvalid#]; name=JustFine
// WITH_INVALID_DOT: End completions

  var y : QuxEnum
  y = .#^UNRESOLVED_1^#
// FIXME: Only contains resolvable ones.
// UNRESOLVED_1:  Begin completions
// UNRESOLVED_1-NOT:  Baz
// UNRESOLVED_1-NOT:  Bar
// UNRESOLVED_1-DAG:  Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     Qux1[#QuxEnum#]; name=Qux1
// UNRESOLVED_1-DAG:  Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     Qux2[#QuxEnum#]; name=Qux2
// UNRESOLVED_1-NOT:  Okay
}

func testUnqualified1(x: QuxEnum) {
  _ = x == .Qux1 || x == .#^UNRESOLVED_2^#Qux2
  // UNRESOLVED_2: Begin completions
  // UNRESOLVED_2-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     Qux1[#QuxEnum#]; name=Qux1
  // UNRESOLVED_2-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]:     Qux2[#QuxEnum#]; name=Qux2
  // UNRESOLVED_2-DAG: Decl[TypeAlias]/CurrNominal:                                RawValue[#Int#]; name=RawValue
  // UNRESOLVED_2-DAG: Decl[Constructor]/CurrNominal:                              init({#rawValue: Int#})[#QuxEnum?#]; name=init(rawValue:)
  // UNRESOLVED_2-DAG: Decl[InstanceMethod]/Super/IsSystem/TypeRelation[Invalid]:  hash({#(self): QuxEnum#})[#(into: inout Hasher) -> Void#]; name=hash(:)
  // UNRESOLVED_2: End completions

  _ = (x == .Qux1#^UNRESOLVED_3^#)
  // UNRESOLVED_3: Begin completions
  // UNRESOLVED_3: End completions

}

struct SomeStruct: Equatable {
  let structMember: String
}

enum OtherEnum {
  case otherEnumCase

  static var otherStaticMember: OtherEnum { fatalError() }
}

enum PlainEnum {
  case caseWithAssociatedType(SomeStruct)

  static var staticMember: PlainEnum { fatalError() }
}

func completeEquatableEnum(x: PlainEnum) {
  switch x {
  case .#^PATTERN_MATCH_PLAIN_ENUM^#
// We shouldn't suggest staticMember here because PlainEnum isn't equatable and thus doesn't have a ~= operator defined between instances
// PATTERN_MATCH_PLAIN_ENUM:     Begin completions, 1 items
// PATTERN_MATCH_PLAIN_ENUM-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: caseWithAssociatedType({#SomeStruct#})[#PlainEnum#];
// PATTERN_MATCH_PLAIN_ENUM:     End completions
  }
}

enum EnumWithCustomPatternMatchingOperator {
  case caseWithAssociatedType(SomeStruct)

  static var staticMember: EnumWithCustomPatternMatchingOperator { fatalError() }
}

func ~=(pattern: OtherEnum, value: EnumWithCustomPatternMatchingOperator) -> Bool {
 return true
}

func completeEnumWithCustomPatternMatchingOperator(x: EnumWithCustomPatternMatchingOperator) {
  switch x {
  case .#^PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING^#
// We should be suggesting static members of `OtherEnum`, because we can match it to `EnumWithCustomPatternMatchingOperator` using the custom pattern match operator. 
// We should also suggest enum cases from `EnumWithCustomPatternMatchingOperator` whose pattern matching doesn't go through any `~=` operator.
// We shouldn't suggest `staticMember` because `EnumWithCustomPatternMatchingOperator` doesn`t have `~=` defined between two of its instances.
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING:     Begin completions, 4 items
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: otherEnumCase[#OtherEnum#];
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: otherStaticMember[#OtherEnum#];
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): OtherEnum#})[#(into: inout Hasher) -> Void#];
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: caseWithAssociatedType({#SomeStruct#})[#EnumWithCustomPatternMatchingOperator#];
// PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING:     End completions
  }
}

func completeEnumAssociatedTypeWithCustomPatternMatchingOperator(x: EnumWithCustomPatternMatchingOperator) {
  switch x {
  case .caseWithAssociatedType(.#^COMPLETE_ASSOCIATED_TYPE_WITH_CUSTOM_PATTERN_MATCHING^#)
// COMPLETE_ASSOCIATED_TYPE_WITH_CUSTOM_PATTERN_MATCHING:     Begin completions, 1 items
// COMPLETE_ASSOCIATED_TYPE_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init({#structMember: String#})[#SomeStruct#];
// COMPLETE_ASSOCIATED_TYPE_WITH_CUSTOM_PATTERN_MATCHING:     End completions
  }
}

enum EquatableEnum: Equatable {
  case caseWithAssociatedType(SomeStruct)

  static var staticMember: EnumWithCustomPatternMatchingOperator { fatalError() }
}

func completeEquatableEnum(x: EquatableEnum) {
  switch x {
  case .#^PATTERN_MATCH_EQUATABLE_ENUM^#
// Suggest all static members of `EquatableEnum` because we can pattern match through the `~=` operator between two equatable types defined in the stdlib.
// PATTERN_MATCH_EQUATABLE_ENUM:     Begin completions, 2 items
// PATTERN_MATCH_EQUATABLE_ENUM-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: caseWithAssociatedType({#SomeStruct#})[#EquatableEnum#];
// PATTERN_MATCH_EQUATABLE_ENUM-DAG: Decl[StaticVar]/CurrNominal:        staticMember[#EnumWithCustomPatternMatchingOperator#];
// PATTERN_MATCH_EQUATABLE_ENUM:     End completions
  }
}

enum EquatableEnumWithCustomPatternMatchingOperator: Equatable {
  case caseWithAssociatedType(SomeStruct)

  static var staticMember: EnumWithCustomPatternMatchingOperator { fatalError() }
}

func ~=(pattern: OtherEnum, value: EquatableEnumWithCustomPatternMatchingOperator) -> Bool {
 return true
}

func completeEnumWithCustomPatternMatchingOperator(x: EquatableEnumWithCustomPatternMatchingOperator) {
  switch x {
  case .#^PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING^#
// Same as PATTERN_MATCH_ENUM_WITH_CUSTOM_PATTERN_MATCHING but also suggest static members on `EquatableEnumWithCustomPatternMatchingOperator` because its `Equatable`
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING:     Begin completions, 5 items
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: otherEnumCase[#OtherEnum#];
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: otherStaticMember[#OtherEnum#];
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): OtherEnum#})[#(into: inout Hasher) -> Void#];
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: caseWithAssociatedType({#SomeStruct#})[#EquatableEnumWithCustomPatternMatchingOperator#];
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING-DAG: Decl[StaticVar]/CurrNominal:        staticMember[#EnumWithCustomPatternMatchingOperator#];
// PATTERN_MATCH_EQUATABLE_ENUM_WITH_CUSTOM_PATTERN_MATCHING:     End completions
  }
}

enum WithNestedEnum {
  case bar(PlainEnum)
}

func completeInNestedPatternMatching(x: WithNestedEnum) {
  switch x {
  case .bar(.#^NESTED_ENUM_PATTERN_MATCHING^#)
}
// NESTED_ENUM_PATTERN_MATCHING:     Begin completions, 1 items
// NESTED_ENUM_PATTERN_MATCHING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: caseWithAssociatedType({#SomeStruct#})[#PlainEnum#];
// NESTED_ENUM_PATTERN_MATCHING:     End completions
