// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// Test code completion of expressions that produce a value.

// NOCRASH: Token

struct FooStruct {
  lazy var lazyInstanceVar = 0
  var instanceVar = 0

  mutating
  func instanceFunc0() {}
  mutating
  func instanceFunc1(_ a: Int) {}
  mutating
  func instanceFunc2(_ a: Int, b: inout Double) {}
  mutating
  func instanceFunc3(_ a: Int, _: (Float, Double)) {}
  mutating
  func instanceFunc4(_ a: Int?, b: Int!, c: inout Int?, d: inout Int!) {}
  mutating
  func instanceFunc5() -> Int? {}
  mutating
  func instanceFunc6() -> Int! {}
  mutating
  func instanceFunc7(a a: Int) {}
  mutating
  func instanceFunc8(_ a: (Int, Int)) {}
  mutating
  func instanceFunc9(a: @autoclosure () -> Int) {}
  mutating
  func instanceFunc10(arg: Int, optArg: Int = 0) {}

  mutating
  func varargInstanceFunc0(_ v: Int...) {}
  mutating
  func varargInstanceFunc1(_ a: Float, v: Int...) {}
  mutating
  func varargInstanceFunc2(_ a: Float, b: Double, v: Int...) {}

  mutating
  func overloadedInstanceFunc1() -> Int {}
  mutating
  func overloadedInstanceFunc1() -> Double {}

  mutating
  func overloadedInstanceFunc2(_ x: Int) -> Int {}
  mutating
  func overloadedInstanceFunc2(_ x: Double) -> Int {}

  mutating
  func builderFunc1(_ a: Int) -> FooStruct { return self }

  subscript(i: Int) -> Double {
    get {
      return Double(i)
    }
    set(v) {
      instanceVar = i
    }
  }

  subscript(i: Int, j: Int) -> Double {
    get {
      return Double(i + j)
    }
    set(v) {
      instanceVar = i + j
    }
  }

  mutating
  func selectorVoidFunc1(_ a: Int, b x: Float) {}
  mutating
  func selectorVoidFunc2(_ a: Int, b x: Float, c y: Double) {}
  mutating
  func selectorVoidFunc3(_ a: Int, b _: (Float, Double)) {}

  mutating
  func selectorStringFunc1(_ a: Int, b x: Float) -> String {}
  mutating
  func selectorStringFunc2(_ a: Int, b x: Float, c y: Double) -> String {}
  mutating
  func selectorStringFunc3(_ a: Int, b _: (Float, Double)) -> String{}

  struct NestedStruct {}
  class NestedClass {}
  enum NestedEnum {}
  // Cannot declare a nested protocol.
  // protocol NestedProtocol {}

  typealias NestedTypealias = Int

  static var staticVar: Int = 4

  static func staticFunc0() {}
  static func staticFunc1(_ a: Int) {}

  static func overloadedStaticFunc1() -> Int {}
  static func overloadedStaticFunc1() -> Double {}

  static func overloadedStaticFunc2(_ x: Int) -> Int {}
  static func overloadedStaticFunc2(_ x: Double) -> Int {}
}

extension FooStruct {
  var extProp: Int {
    get {
      return 42
    }
    set(v) {}
  }

  mutating
  func extFunc0() {}

  static var extStaticProp: Int {
    get {
      return 42
    }
    set(v) {}
  }

  static func extStaticFunc0() {}

  struct ExtNestedStruct {}
  class ExtNestedClass {}
  enum ExtNestedEnum {
    case ExtEnumX(Int)
  }

  typealias ExtNestedTypealias = Int
}

var fooObject: FooStruct

// FOO_OBJECT_DOT-DAG: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// FOO_OBJECT_DOT-DAG: Decl[InstanceVar]/CurrNominal:    lazyInstanceVar[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceVar]/CurrNominal:    instanceVar[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc0()[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc2({#(a): Int#}, {#b: &Double#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc3({#(a): Int#}, {#(Float, Double)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc4({#(a): Int?#}, {#b: Int!#}, {#c: &Int?#}, {#d: &Int!#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc5()[#Int?#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc6()[#Int!#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc7({#a: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc8({#(a): (Int, Int)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc9({#a: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc10({#arg: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc10({#arg: Int#}, {#optArg: Int#})[#Void#]{{; name=.+$}}
//
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: varargInstanceFunc0({#(v): Int...#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: varargInstanceFunc1({#(a): Float#}, {#v: Int...#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: varargInstanceFunc2({#(a): Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{; name=.+$}}
//
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1()[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1()[#Double#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(x): Int#})[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(x): Double#})[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: builderFunc1({#(a): Int#})[#FooStruct#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: selectorVoidFunc1({#(a): Int#}, {#b: Float#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: selectorVoidFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: selectorVoidFunc3({#(a): Int#}, {#b: (Float, Double)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc1({#(a): Int#}, {#b: Float#})[#String#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#String#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc3({#(a): Int#}, {#b: (Float, Double)#})[#String#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceVar]/CurrNominal:    extProp[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: extFunc0()[#Void#]{{; name=.+$}}

// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceVar]/CurrNominal:    .lazyInstanceVar[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceVar]/CurrNominal:    .instanceVar[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc0()[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc2({#(a): Int#}, {#b: &Double#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc3({#(a): Int#}, {#(Float, Double)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc4({#(a): Int?#}, {#b: Int!#}, {#c: &Int?#}, {#d: &Int!#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc5()[#Int?#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc6()[#Int!#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc7({#a: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc8({#(a): (Int, Int)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc9({#a: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc10({#arg: Int#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc10({#arg: Int#}, {#optArg: Int#})[#Void#]{{; name=.+$}}
//
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc0({#(v): Int...#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc1({#(a): Float#}, {#v: Int...#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc2({#(a): Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{; name=.+$}}
//
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1()[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1()[#Double#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(x): Int#})[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(x): Double#})[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .builderFunc1({#(a): Int#})[#FooStruct#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[Subscript]/CurrNominal:      [{#(i): Int#}][#Double#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[Subscript]/CurrNominal:      [{#(i): Int#}, {#(j): Int#}][#Double#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc1({#(a): Int#}, {#b: Float#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc3({#(a): Int#}, {#b: (Float, Double)#})[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc1({#(a): Int#}, {#b: Float#})[#String#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#String#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc3({#(a): Int#}, {#b: (Float, Double)#})[#String#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceVar]/CurrNominal:    .extProp[#Int#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .extFunc0()[#Void#]{{; name=.+$}}
// FOO_OBJECT_NO_DOT-DAG: BuiltinOperator/None:                     = {#Foo
// FOO_OBJECT_NO_DOT-DAG: Keyword[self]/CurrNominal: .self[#FooStruct#]; name=self

// FOO_STRUCT_DOT-DAG: Keyword[self]/CurrNominal: self[#FooStruct.Type#]; name=self
// FOO_STRUCT_DOT-DAG: Keyword/CurrNominal: Type[#FooStruct.Type#]; name=Type
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc0({#(self): &FooStruct#})[#() -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc1({#(self): &FooStruct#})[#(Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc2({#(self): &FooStruct#})[#(Int, b: inout Double) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc3({#(self): &FooStruct#})[#(Int, (Float, Double)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc4({#(self): &FooStruct#})[#(Int?, b: Int?, c: inout Int?, d: inout Int?) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc5({#(self): &FooStruct#})[#() -> Int?#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc6({#(self): &FooStruct#})[#() -> Int?#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc7({#(self): &FooStruct#})[#(a: Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc8({#(self): &FooStruct#})[#((Int, Int)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc9({#(self): &FooStruct#})[#(a: @autoclosure () -> Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc10({#(self): &FooStruct#})[#(arg: Int, optArg: Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc0({#(self): &FooStruct#})[#(Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc1({#(self): &FooStruct#})[#(Float, v: Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc2({#(self): &FooStruct#})[#(Float, b: Double, v: Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1({#(self): &FooStruct#})[#() -> Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1({#(self): &FooStruct#})[#() -> Double#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(self): &FooStruct#})[#(Int) -> Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(self): &FooStruct#})[#(Double) -> Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: builderFunc1({#(self): &FooStruct#})[#(Int) -> FooStruct#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc1({#(self): &FooStruct#})[#(Int, b: Float) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc2({#(self): &FooStruct#})[#(Int, b: Float, c: Double) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc3({#(self): &FooStruct#})[#(Int, b: (Float, Double)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc1({#(self): &FooStruct#})[#(Int, b: Float) -> String#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc2({#(self): &FooStruct#})[#(Int, b: Float, c: Double) -> String#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: selectorStringFunc3({#(self): &FooStruct#})[#(Int, b: (Float, Double)) -> String#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Struct]/CurrNominal:         NestedStruct[#FooStruct.NestedStruct#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Class]/CurrNominal:          NestedClass[#FooStruct.NestedClass#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Enum]/CurrNominal:           NestedEnum[#FooStruct.NestedEnum#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[TypeAlias]/CurrNominal:      NestedTypealias[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   staticFunc0()[#Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   staticFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc1()[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc1()[#Double#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc2({#(x): Int#})[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc2({#(x): Double#})[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Constructor]/CurrNominal:    init()[#FooStruct#]; name=init(){{$}}
// FOO_STRUCT_DOT-DAG: Decl[Constructor]/CurrNominal:    init({#lazyInstanceVar: Int?#}, {#instanceVar: Int#})[#FooStruct#]; name=init(lazyInstanceVar:instanceVar:){{$}}
// FOO_STRUCT_DOT-DAG: Decl[Constructor]/CurrNominal:    init()[#FooStruct#]; name=init(){{$}}
// FOO_STRUCT_DOT-DAG: Decl[InstanceMethod]/CurrNominal: extFunc0({#(self): &FooStruct#})[#() -> Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticVar]/CurrNominal:      extStaticProp[#Int#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[StaticMethod]/CurrNominal:   extStaticFunc0()[#Void#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Struct]/CurrNominal:         ExtNestedStruct[#FooStruct.ExtNestedStruct#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Class]/CurrNominal:          ExtNestedClass[#FooStruct.ExtNestedClass#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[Enum]/CurrNominal:           ExtNestedEnum[#FooStruct.ExtNestedEnum#]{{; name=.+$}}
// FOO_STRUCT_DOT-DAG: Decl[TypeAlias]/CurrNominal:      ExtNestedTypealias[#Int#]{{; name=.+$}}

// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc0({#(self): &FooStruct#})[#() -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc1({#(self): &FooStruct#})[#(Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc2({#(self): &FooStruct#})[#(Int, b: inout Double) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc3({#(self): &FooStruct#})[#(Int, (Float, Double)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc4({#(self): &FooStruct#})[#(Int?, b: Int?, c: inout Int?, d: inout Int?) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc5({#(self): &FooStruct#})[#() -> Int?#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc6({#(self): &FooStruct#})[#() -> Int?#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc7({#(self): &FooStruct#})[#(a: Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc8({#(self): &FooStruct#})[#((Int, Int)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc9({#(self): &FooStruct#})[#(a: @autoclosure () -> Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc10({#(self): &FooStruct#})[#(arg: Int, optArg: Int) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc0({#(self): &FooStruct#})[#(Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc1({#(self): &FooStruct#})[#(Float, v: Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc2({#(self): &FooStruct#})[#(Float, b: Double, v: Int...) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1({#(self): &FooStruct#})[#() -> Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1({#(self): &FooStruct#})[#() -> Double#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(self): &FooStruct#})[#(Int) -> Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(self): &FooStruct#})[#(Double) -> Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .builderFunc1({#(self): &FooStruct#})[#(Int) -> FooStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc1({#(self): &FooStruct#})[#(Int, b: Float) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc2({#(self): &FooStruct#})[#(Int, b: Float, c: Double) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc3({#(self): &FooStruct#})[#(Int, b: (Float, Double)) -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc1({#(self): &FooStruct#})[#(Int, b: Float) -> String#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc2({#(self): &FooStruct#})[#(Int, b: Float, c: Double) -> String#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc3({#(self): &FooStruct#})[#(Int, b: (Float, Double)) -> String#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Struct]/CurrNominal:         .NestedStruct[#FooStruct.NestedStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Class]/CurrNominal:          .NestedClass[#FooStruct.NestedClass#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Enum]/CurrNominal:           .NestedEnum[#FooStruct.NestedEnum#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[TypeAlias]/CurrNominal:      .NestedTypealias[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .staticFunc0()[#Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .staticFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc1()[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc1()[#Double#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc2({#(x): Int#})[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc2({#(x): Double#})[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#lazyInstanceVar: Int?#}, {#instanceVar: Int#})[#FooStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .extFunc0({#(self): &FooStruct#})[#() -> Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticVar]/CurrNominal:      .extStaticProp[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[StaticMethod]/CurrNominal:   .extStaticFunc0()[#Void#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Struct]/CurrNominal:         .ExtNestedStruct[#FooStruct.ExtNestedStruct#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Class]/CurrNominal:          .ExtNestedClass[#FooStruct.ExtNestedClass#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[Enum]/CurrNominal:           .ExtNestedEnum[#FooStruct.ExtNestedEnum#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Decl[TypeAlias]/CurrNominal:      .ExtNestedTypealias[#Int#]{{; name=.+$}}
// FOO_STRUCT_NO_DOT-DAG: Keyword[self]/CurrNominal:        .self[#FooStruct.Type#]; name=self
// FOO_STRUCT_NO_DOT-DAG: Keyword/CurrNominal:              .Type[#FooStruct.Type#]; name=Type

func testObjectExpr() {
  fooObject.#^FOO_OBJECT_DOT_1?check=FOO_OBJECT_DOT^#
}

func testDotDotTokenSplitWithCodeCompletion() {
  fooObject.#^FOO_OBJECT_DOT_2?check=FOO_OBJECT_DOT^#.bar
}

func testObjectExprBuilderStyle1() {
  fooObject
    .#^FOO_OBJECT_DOT_3?check=FOO_OBJECT_DOT^#
}

func testObjectExprBuilderStyle2() {
  fooObject
    .builderFunc1(42).#^FOO_OBJECT_DOT_4?check=FOO_OBJECT_DOT^#
}

func testObjectExprBuilderStyle3() {
  fooObject
    .builderFunc1(42)
    .#^FOO_OBJECT_DOT_5?check=FOO_OBJECT_DOT^#
}

func testObjectExprWithoutDot() {
  fooObject#^FOO_OBJECT_NO_DOT_1?check=FOO_OBJECT_NO_DOT^#
}

func testObjectExprWithoutSpaceAfterCodeCompletion() {
  fooObject#^FOO_OBJECT_NO_DOT_2?check=FOO_OBJECT_NO_DOT^#.bar
}

func testMetatypeExpr() {
  FooStruct.#^FOO_STRUCT_DOT_1?check=FOO_STRUCT_DOT^#
}

func testMetatypeExprWithoutDot() {
  FooStruct#^FOO_STRUCT_NO_DOT_1?check=FOO_STRUCT_NO_DOT^#
}

struct NoMetaCompletions {
  static var foo: Int = 0
  class func bar() {}
  typealias Foo = Int
}
func testMetatypeCompletions() {
  NoMetaCompletions.Type.#^FOO_STRUCT_META_1^#
}
// FOO_STRUCT_META_1: Begin completions, 2 items
// FOO_STRUCT_META_1-DAG: Keyword[self]/CurrNominal:          self[#NoMetaCompletions.Type.Type#]; name=self
// FOO_STRUCT_META_1-DAG: Keyword/CurrNominal:                Type[#NoMetaCompletions.Type.Type#]; name=Type
// FOO_STRUCT_META_1: End completions
func testMetatypeCompletionsWithoutDot() {
  NoMetaCompletions.Type#^FOO_STRUCT_META_2?check=FOO_STRUCT_META^#
}
// FOO_STRUCT_META-NOT: Decl
// FOO_STRUCT_META: Keyword[self]/CurrNominal: {{self|.self}}[#NoMetaCompletions.Type.Type#]; name=self
// FOO_STRUCT_META-NOT: Decl

func testImplicitlyCurriedFunc(_ fs: inout FooStruct) {
  FooStruct.instanceFunc0(&fs)#^IMPLICITLY_CURRIED_FUNC_0^#
// IMPLICITLY_CURRIED_FUNC_0-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ()[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_FUNC_0-DAG: Keyword[self]/CurrNominal: .self[#() -> ()#]; name=self

  FooStruct.instanceFunc1(&fs)#^IMPLICITLY_CURRIED_FUNC_1^#
// IMPLICITLY_CURRIED_FUNC_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(a): Int#})[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_FUNC_1-DAG: Keyword[self]/CurrNominal: .self[#(Int) -> ()#]; name=self

  FooStruct.instanceFunc2(&fs)#^IMPLICITLY_CURRIED_FUNC_2^#
// IMPLICITLY_CURRIED_FUNC_2-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(a): Int#}, {#b: &Double#})[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_FUNC_2-DAG: Keyword[self]/CurrNominal: .self[#(Int, inout Double) -> ()#]; name=self

  FooStruct.varargInstanceFunc0(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_0^#
// IMPLICITLY_CURRIED_VARARG_FUNC_0-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(v): Int...#})[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_0-DAG: Keyword[self]/CurrNominal: .self[#(Int...) -> ()#]; name=self

  FooStruct.varargInstanceFunc1(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_1^#
// IMPLICITLY_CURRIED_VARARG_FUNC_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(a): Float#}, {#v: Int...#})[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_1-DAG: Keyword[self]/CurrNominal: .self[#(Float, Int...) -> ()#]; name=self

  FooStruct.varargInstanceFunc2(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_2^#
// IMPLICITLY_CURRIED_VARARG_FUNC_2-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(a): Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{; name=.+$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_2-DAG: Keyword[self]/CurrNominal: .self[#(Float, Double, Int...) -> ()#]; name=self

  // This call is ambiguous, and the expression is invalid.
  // Ensure that we don't suggest to call the result.
  FooStruct.overloadedInstanceFunc1(&fs)#^IMPLICITLY_CURRIED_OVERLOADED_FUNC_1^#
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1: Begin completions, 4 items
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1-DAG: Keyword[self]/CurrNominal:          .self[#() -> Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ()[#Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1-DAG: Keyword[self]/CurrNominal:          .self[#() -> Double#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ()[#Double#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1: End completions

  // This call is ambiguous, and the expression is invalid.
  // Ensure that we don't suggest to call the result.
  FooStruct.overloadedInstanceFunc2(&fs)#^IMPLICITLY_CURRIED_OVERLOADED_FUNC_2^#
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2: Begin completions, 4 items
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2-DAG: Keyword[self]/CurrNominal:          .self[#(Int) -> Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(x): Int#})[#Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2-DAG: Keyword[self]/CurrNominal:          .self[#(Double) -> Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(x): Double#})[#Int#];
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2: End completions
}

//===---
//===--- Test that we can complete inside 'case'.
//===---

func testSwitch1() {
  switch fooObject {
    case #^IN_SWITCH_CASE_1?check=IN_SWITCH_CASE^#
  }

  switch fooObject {
    case 1, #^IN_SWITCH_CASE_2?check=IN_SWITCH_CASE^#
  }

  switch unknown_var {
    case #^IN_SWITCH_CASE_3?check=IN_SWITCH_CASE^#
  }

  switch {
    case #^IN_SWITCH_CASE_4?check=IN_SWITCH_CASE^#
  }
}

// IN_SWITCH_CASE-DAG: Decl[GlobalVar]/CurrModule{{(/TypeRelation\[Convertible\])?}}: fooObject[#FooStruct#]{{; name=.+$}}
// IN_SWITCH_CASE-DAG: Decl[Struct]/CurrModule{{(/TypeRelation\[Convertible\])?}}:    FooStruct[#FooStruct#]{{; name=.+$}}

//===--- Helper types that are used in this test

struct FooGenericStruct<T> {
  init() {}
  init(t: T) { fooInstanceVarT = t }

  var fooInstanceVarT: T
  var fooInstanceVarTBrackets: [T]
  mutating
  func fooVoidInstanceFunc1(_ a: T) {}
  mutating
  func fooTInstanceFunc1(_ a: T) -> T { return a }
  mutating
  func fooUInstanceFunc1<U>(_ a: U) -> U { return a }

  static var fooStaticVarT: Int = 0
  static var fooStaticVarTBrackets: [Int] = [0]
  static func fooVoidStaticFunc1(_ a: T) {}
  static func fooTStaticFunc1(_ a: T) -> T { return a }
  static func fooUInstanceFunc1<U>(_ a: U) -> U { return a }
}

class FooClass {
  var fooClassInstanceVar = 0
  func fooClassInstanceFunc0() {}
  func fooClassInstanceFunc1(_ a: Int) {}
}

enum FooEnum {
}

protocol FooProtocol {
  var fooInstanceVar1: Int { get set }
  var fooInstanceVar2: Int { get }
  typealias FooTypeAlias1
  func fooInstanceFunc0() -> Double
  func fooInstanceFunc1(_ a: Int) -> Double
  subscript(i: Int) -> Double { get set }
}

class FooProtocolImpl : FooProtocol {
  var fooInstanceVar1 = 0
  val fooInstanceVar2 = 0
  typealias FooTypeAlias1 = Float
  init() {}
  func fooInstanceFunc0() -> Double {
    return 0.0
  }
  func fooInstanceFunc1(_ a: Int) -> Double {
    return Double(a)
  }
  subscript(i: Int) -> Double {
    return 0.0
  }
}

protocol FooExProtocol : FooProtocol {
  func fooExInstanceFunc0() -> Double
}

protocol BarProtocol {
  var barInstanceVar: Int { get set }
  typealias BarTypeAlias1
  func barInstanceFunc0() -> Double
  func barInstanceFunc1(_ a: Int) -> Double
}

protocol BarExProtocol : BarProtocol {
  func barExInstanceFunc0() -> Double
}

protocol BazProtocol {
  func bazInstanceFunc0() -> Double
}

typealias BarBazProtocolComposition = BarProtocol & BazProtocol

let fooProtocolInstance: FooProtocol = FooProtocolImpl()
let fooBarProtocolInstance: FooProtocol & BarProtocol
let fooExBarExProtocolInstance: FooExProtocol & BarExProtocol

typealias FooTypealias = Int

//===--- Test that we can code complete inside function calls.

func testInsideFunctionCall0() {
  ERROR(#^INSIDE_FUNCTION_CALL_0^#
// INSIDE_FUNCTION_CALL_0-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideFunctionCall1() {
  var a = FooStruct()
  a.instanceFunc0(#^INSIDE_FUNCTION_CALL_1^#
// INSIDE_FUNCTION_CALL_1: Begin completions, 1 items
// INSIDE_FUNCTION_CALL_1: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['('][')'][#Void#]; name=
}

func testInsideFunctionCall2() {
  var a = FooStruct()
  a.instanceFunc1(#^INSIDE_FUNCTION_CALL_2^#
// INSIDE_FUNCTION_CALL_2-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): Int#}[')'][#Void#]{{; name=.+$}}
// INSIDE_FUNCTION_CALL_2-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideFunctionCall3() {
  FooStruct().instanceFunc1(42, #^INSIDE_FUNCTION_CALL_3^#
// INSIDE_FUNCTION_CALL_3-NOT: Begin Completions
}

func testInsideFunctionCall4() {
  var a = FooStruct()
  a.instanceFunc2(#^INSIDE_FUNCTION_CALL_4^#
// INSIDE_FUNCTION_CALL_4-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): Int#}, {#b: &Double#}[')'][#Void#]{{; name=.+$}}
// INSIDE_FUNCTION_CALL_4-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideFunctionCall5() {
  FooStruct().instanceFunc2(42, #^INSIDE_FUNCTION_CALL_5^#
// INSIDE_FUNCTION_CALL_5-DAG: Pattern/Local/Flair[ArgLabels]: {#b: &Double#}[#inout Double#];
}

func testInsideFunctionCall6() {
  var a = FooStruct()
  a.instanceFunc7(#^INSIDE_FUNCTION_CALL_6^#
// INSIDE_FUNCTION_CALL_6-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#a: Int#}[')'][#Void#]{{; name=.+$}}
}

func testInsideFunctionCall7() {
  var a = FooStruct()
  a.instanceFunc8(#^INSIDE_FUNCTION_CALL_7^#
// INSIDE_FUNCTION_CALL_7: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): (Int, Int)#}[')'][#Void#]{{; name=.+$}}
}

func testInsideFunctionCall8(_ x: inout FooStruct) {
  x.instanceFunc0(#^INSIDE_FUNCTION_CALL_8^#)
// INSIDE_FUNCTION_CALL_8: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['('][')'][#Void#]; name=
}
func testInsideFunctionCall9(_ x: inout FooStruct) {
  x.instanceFunc1(#^INSIDE_FUNCTION_CALL_9^#)
// Annotated ')'
// INSIDE_FUNCTION_CALL_9-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): Int#}[')'][#Void#]{{; name=.+$}}
// INSIDE_FUNCTION_CALL_9-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}
func testInsideFunctionCall10(_ x: inout FooStruct) {
  x.instanceFunc2(#^INSIDE_FUNCTION_CALL_10^#)
// Annotated ')'
// INSIDE_FUNCTION_CALL_10-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(a): Int#}, {#b: &Double#}[')'][#Void#]{{; name=.+$}}
// INSIDE_FUNCTION_CALL_10-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}
func testInsideFunctionCall11(_ x: inout FooStruct) {
  x.instanceFunc2(#^INSIDE_FUNCTION_CALL_11?check=INSIDE_FUNCTION_CALL_4^#,
// INSIDE_FUNCTION_CALL_11-NOT: Pattern/{{.*}}:{{.*}}({{.*}}{#Int#}
// INSIDE_FUNCTION_CALL_11B: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#Int#}, {#b: &Double#}[')'][#Void#];
}
func testInsideFunctionCall12(_ x: inout FooStruct) {
  x.instanceFunc2(#^INSIDE_FUNCTION_CALL_12?check=INSIDE_FUNCTION_CALL_4^#<#placeholder#>
// INSIDE_FUNCTION_CALL_12-NOT: Decl[InstanceMethod]/{{.*}}:{{.*}}({{.*}}{#Int#}
}

func testInsideVarargFunctionCall1() {
  var a = FooStruct()
  a.varargInstanceFunc0(#^INSIDE_VARARG_FUNCTION_CALL_1^#
// INSIDE_VARARG_FUNCTION_CALL_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(v): Int...#}[')'][#Void#]{{; name=.+$}}
// INSIDE_VARARG_FUNCTION_CALL_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideVarargFunctionCall2() {
  FooStruct().varargInstanceFunc0(42, #^INSIDE_VARARG_FUNCTION_CALL_2^#
// INSIDE_VARARG_FUNCTION_CALL_2-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideVarargFunctionCall3() {
  FooStruct().varargInstanceFunc0(42, 4242, #^INSIDE_VARARG_FUNCTION_CALL_3^#
// INSIDE_VARARG_FUNCTION_CALL_3-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideOverloadedFunctionCall1() {
  var a = FooStruct()
  a.overloadedInstanceFunc2(#^INSIDE_OVERLOADED_FUNCTION_CALL_1^#
// FIXME: produce call patterns here.
// INSIDE_OVERLOADED_FUNCTION_CALL_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

func testInsideFunctionCallOnClassInstance1(_ a: FooClass) {
  a.fooClassInstanceFunc1(#^INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1^#
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:       ['(']{#(a): Int#}[')'][#Void#]{{; name=.+$}}
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}
}

//===--- Variables that have function types.

class FuncTypeVars {
  var funcVar1: () -> Double
  var funcVar2: (a: Int) -> Double // adding the label is erroneous.
}

var funcTypeVarsObject: FuncTypeVars
func testFuncTypeVars() {
  funcTypeVarsObject.funcVar1#^VF1^#
// VF1: Begin completions, 3 items
// VF1-DAG: Pattern/CurrModule/Flair[ArgLabels]: ()[#Double#]{{; name=.+$}}
// VF1-DAG: BuiltinOperator/None:         = {#() -> Double##() -> Double#}
// VF1-DAG: Keyword[self]/CurrNominal:    .self[#() -> Double#]; name=self

  funcTypeVarsObject.funcVar2#^VF2^#
// VF2: Begin completions, 3 items
// VF2-DAG: Pattern/CurrModule/Flair[ArgLabels]: ({#Int#})[#Double#]{{; name=.+$}}
// VF2-DAG: BuiltinOperator/None:         = {#(Int) -> Double##(_ a: Int) -> Double#}
// VF2-DAG: Keyword[self]/CurrNominal:    .self[#(Int) -> Double#]; name=self
}

//===--- Check that we look into base classes.

class MembersBase {
  var baseVar = 0
  func baseInstanceFunc() {}
  class func baseStaticFunc() {}
}

class MembersDerived : MembersBase {
  var derivedVar = 0
  func derivedInstanceFunc() {}
  class func derivedStaticFunc() {}
}

var membersDerived: MembersDerived
func testLookInBase() {
  membersDerived.#^BASE_MEMBERS^#
// BASE_MEMBERS-DAG: Keyword[self]/CurrNominal: self[#MembersDerived#]; name=self
// BASE_MEMBERS-DAG: Decl[InstanceVar]/CurrNominal:    derivedVar[#Int#]{{; name=.+$}}
// BASE_MEMBERS-DAG: Decl[InstanceMethod]/CurrNominal: derivedInstanceFunc()[#Void#]{{; name=.+$}}
// BASE_MEMBERS-DAG: Decl[InstanceVar]/Super:          baseVar[#Int#]{{; name=.+$}}
// BASE_MEMBERS-DAG: Decl[InstanceMethod]/Super:       baseInstanceFunc()[#Void#]{{; name=.+$}}
}

func testLookInBaseStatic() {
  MembersDerived.#^BASE_MEMBERS_STATIC^#
// BASE_MEMBERS_STATIC-DAG: Keyword[self]/CurrNominal: self[#MembersDerived.Type#]; name=self
// BASE_MEMBERS_STATIC-DAG: Keyword/CurrNominal: Type[#MembersDerived.Type#]; name=Type
// BASE_MEMBERS_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: derivedInstanceFunc({#(self): MembersDerived#})[#() -> Void#]{{; name=.+$}}
// BASE_MEMBERS_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   derivedStaticFunc()[#Void#]{{; name=.+$}}
// BASE_MEMBERS_STATIC-DAG: Decl[Constructor]/CurrNominal:    init()[#MembersDerived#]; name=init(){{$}}
// BASE_MEMBERS_STATIC-DAG: Decl[InstanceMethod]/Super:       baseInstanceFunc({#(self): MembersBase#})[#() -> Void#]{{; name=.+$}}
// BASE_MEMBERS_STATIC-DAG: Decl[StaticMethod]/Super:         baseStaticFunc()[#Void#]{{; name=.+$}}
}

//===--- Check that we can look into protocols.

func testLookInProtoNoDot1() {
  fooProtocolInstance#^PROTO_MEMBERS_NO_DOT_1^#
// PROTO_MEMBERS_NO_DOT_1-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_1-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_1-DAG: Decl[Subscript]/CurrNominal:      [{#(i): Int#}][#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_1-DAG: Keyword[self]/CurrNominal:        .self[#any FooProtocol#]; name=self
}

func testLookInProtoNoDot2() {
  fooBarProtocolInstance#^PROTO_MEMBERS_NO_DOT_2^#
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceVar]/CurrNominal:    .barInstanceVar[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceMethod]/CurrNominal: .barInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceMethod]/CurrNominal: .barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Decl[Subscript]/CurrNominal:      [{#(i): Int#}][#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_2-DAG: Keyword[self]/CurrNominal: .self[#any BarProtocol & FooProtocol#]; name=self
}

func testLookInProtoNoDot3() {
  fooExBarExProtocolInstance#^PROTO_MEMBERS_NO_DOT_3^#
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/CurrNominal: .barExInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceVar]/Super:          .barInstanceVar[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/Super:       .barInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/Super:       .barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/CurrNominal: .fooExInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceVar]/Super:          .fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceVar]/Super:          .fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/Super:       .fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[InstanceMethod]/Super:       .fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Decl[Subscript]/Super:            [{#(i): Int#}][#Double#]{{; name=.+$}}
// PROTO_MEMBERS_NO_DOT_3-DAG: Keyword[self]/CurrNominal: .self[#any BarExProtocol & FooExProtocol#]; name=self
}

func testLookInProto1() {
  fooProtocolInstance.#^PROTO_MEMBERS_1^#
// PROTO_MEMBERS_1-DAG: Keyword[self]/CurrNominal: self[#any FooProtocol#]; name=self
// PROTO_MEMBERS_1-DAG: Decl[InstanceVar]/CurrNominal:    fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_1-DAG: Decl[InstanceVar]/CurrNominal:    fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_1-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_1-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testLookInProto2() {
  fooBarProtocolInstance.#^PROTO_MEMBERS_2^#
// PROTO_MEMBERS_2-DAG: Keyword[self]/CurrNominal: self[#any BarProtocol & FooProtocol#]; name=self
// PROTO_MEMBERS_2-DAG: Decl[InstanceVar]/CurrNominal:    barInstanceVar[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceVar]/CurrNominal:    fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceVar]/CurrNominal:    fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_2-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testLookInProto3() {
  fooExBarExProtocolInstance.#^PROTO_MEMBERS_3^#
// PROTO_MEMBERS_3-DAG: Keyword[self]/CurrNominal: self[#any BarExProtocol & FooExProtocol#]; name=self
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/CurrNominal: barExInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceVar]/Super:          barInstanceVar[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/Super:       barInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/Super:       barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/CurrNominal: fooExInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceVar]/Super:          fooInstanceVar1[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceVar]/Super:          fooInstanceVar2[#Int#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/Super:       fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_3-DAG: Decl[InstanceMethod]/Super:       fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testLookInProto4(_ a: FooProtocol & BarBazProtocolComposition) {
  a.#^PROTO_MEMBERS_4^#
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc0()[#Double#]{{; name=.+$}}
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc0()[#Double#]{{; name=.+$}}
}

//===--- Check that we can resolve function parameters.

func testResolveFuncParam1(_ fs: FooStruct) {
  fs.#^RESOLVE_FUNC_PARAM_1?check=FOO_OBJECT_DOT^#
}

class TestResolveFuncParam2 {
  func testResolveFuncParam2a(_ fs: FooStruct) {
    fs.#^RESOLVE_FUNC_PARAM_2?check=FOO_OBJECT_DOT^#
  }
}

func testResolveFuncParam3<Foo : FooProtocol>(_ foo: Foo) {
  foo.#^RESOLVE_FUNC_PARAM_3^#
// RESOLVE_FUNC_PARAM_3-DAG: Keyword[self]/CurrNominal: self[#Foo#]; name=self
// RESOLVE_FUNC_PARAM_3-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_3-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_3-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_3-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testResolveFuncParam4<FooBar : FooProtocol & BarProtocol>(_ fooBar: FooBar) {
  fooBar.#^RESOLVE_FUNC_PARAM_4^#
// RESOLVE_FUNC_PARAM_4-DAG: Keyword[self]/CurrNominal: self[#FooBar#]; name=self
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceVar]/CurrNominal:    barInstanceVar[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_4-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testResolveFuncParam5<FooExBarEx : FooExProtocol & BarExProtocol>(_ a: FooExBarEx) {
  a.#^RESOLVE_FUNC_PARAM_5^#
// RESOLVE_FUNC_PARAM_5-DAG: Keyword[self]/CurrNominal: self[#FooExBarEx#]; name=self
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/CurrNominal: barExInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceVar]/Super: barInstanceVar[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/Super: barInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/Super: barInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/CurrNominal: fooExInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceVar]/Super: fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceVar]/Super: fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_5-DAG: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
}

func testResolveFuncParam6<Foo : FooProtocol where Foo : FooClass>(_ foo: Foo) {
  foo.#^RESOLVE_FUNC_PARAM_6^#
// RESOLVE_FUNC_PARAM_6-DAG: Keyword[self]/CurrNominal: self[#Foo#]; name=self
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceVar]/CurrNominal: fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceVar]/CurrNominal: fooClassInstanceVar[#Int#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceMethod]/CurrNominal: fooClassInstanceFunc0()[#Void#]{{; name=.+$}}
// RESOLVE_FUNC_PARAM_6-DAG: Decl[InstanceMethod]/CurrNominal: fooClassInstanceFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
}

class TestResolveConstructorParam1 {
  init(fs: FooStruct) {
    fs.#^RESOLVE_CONSTRUCTOR_PARAM_1?check=FOO_OBJECT_DOT^#
  }
}

class TestResolveConstructorParam2 {
  init<Foo : FooProtocol>(foo: Foo) {
    foo.#^RESOLVE_CONSTRUCTOR_PARAM_2^#
// RESOLVE_CONSTRUCTOR_PARAM_2-DAG: Keyword[self]/CurrNominal: self[#Foo#]; name=self
// RESOLVE_CONSTRUCTOR_PARAM_2-DAG: Decl[InstanceVar]/CurrNominal:  fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-DAG: Decl[InstanceVar]/CurrNominal:  fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
  }
}

class TestResolveConstructorParam3<Foo : FooProtocol> {
  init(foo: Foo) {
    foo.#^RESOLVE_CONSTRUCTOR_PARAM_3^#
// RESOLVE_CONSTRUCTOR_PARAM_3-DAG: Keyword[self]/CurrNominal: self[#Foo#]; name=self
// RESOLVE_CONSTRUCTOR_PARAM_3-DAG: Decl[InstanceVar]/CurrNominal:  fooInstanceVar1[#Int#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-DAG: Decl[InstanceVar]/CurrNominal:  fooInstanceVar2[#Int#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{; name=.+$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{; name=.+$}}
  }
}

//===--- Check that we can handle ParenPattern in function arguments.

struct FuncParenPattern {
  init(_: Int) {}
  init(_: (Int, Int)) {}

  mutating
  func instanceFunc(_: Int) {}

  subscript(_: Int) -> Int {
    get {
      return 0
    }
  }
}

func testFuncParenPattern1(_ fpp: FuncParenPattern) {
  fpp#^FUNC_PAREN_PATTERN_1^#
// FUNC_PAREN_PATTERN_1-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#Int#})[#Void#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_1-DAG: Decl[Subscript]/CurrNominal: [{#Int#}][#Int#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_1-DAG: Keyword[self]/CurrNominal: .self[#FuncParenPattern#]; name=self
}

func testFuncParenPattern2(_ fpp: FuncParenPattern) {
  FuncParenPattern#^FUNC_PAREN_PATTERN_2^#
// FUNC_PAREN_PATTERN_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#Int#})[#FuncParenPattern#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#(Int, Int)#})[#FuncParenPattern#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_2-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#(self): &FuncParenPattern#})[#(Int) -> Void#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_2-DAG: Keyword[self]/CurrNominal: .self[#FuncParenPattern.Type#]; name=self
// FUNC_PAREN_PATTERN_2-DAG: Keyword/CurrNominal: .Type[#FuncParenPattern.Type#]; name=Type
}

func testFuncParenPattern3(_ fpp: inout FuncParenPattern) {
  fpp.instanceFunc#^FUNC_PAREN_PATTERN_3^#
// FUNC_PAREN_PATTERN_3-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#Int#})[#Void#]{{; name=.+$}}
// FUNC_PAREN_PATTERN_3-DAG: Keyword[self]/CurrNominal: .self[#(Int) -> ()#]; name=self
}

//===--- Check that we can code complete after function calls

struct SomeBuilder {
  init(_ a: Int) {}
  func doFoo() -> SomeBuilder { return self }
  func doBar() -> SomeBuilder { return self }
  func doBaz(_ z: Double) -> SomeBuilder { return self }
}

func testChainedCalls1() {
  SomeBuilder(42)#^CHAINED_CALLS_1^#
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#(z): Double#})[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_1-DAG: Keyword[self]/CurrNominal: .self[#SomeBuilder#]; name=self
}

func testChainedCalls2() {
  SomeBuilder(42).doFoo()#^CHAINED_CALLS_2^#
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#(z): Double#})[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_2-DAG: Keyword[self]/CurrNominal: .self[#SomeBuilder#]; name=self
}

func testChainedCalls3() {
  // doBaz() takes a Double.  Check that we can recover.
  SomeBuilder(42).doFoo().doBaz(SomeBuilder(24))#^CHAINED_CALLS_3?xfail=FIXME^#
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#z: Double#})[#SomeBuilder#]{{; name=.+$}}
// CHAINED_CALLS_3-DAG: Keyword[self]/CurrNominal: .self[#SomeBuilder#]; name=self
}

//===--- Check that we can code complete expressions that have generic parameters

func testResolveGenericParams1() {
  FooGenericStruct<FooStruct>()#^RESOLVE_GENERIC_PARAMS_1^#
// RESOLVE_GENERIC_PARAMS_1-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[FooStruct]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): FooStruct#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): FooStruct#})[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<FooStruct>#]; name=self

  FooGenericStruct<FooStruct>#^RESOLVE_GENERIC_PARAMS_1_STATIC^#
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooGenericStruct<FooStruct>#]; name=()
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#t: FooStruct#})[#FooGenericStruct<FooStruct>#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(FooStruct) -> Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(FooStruct) -> FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): FooStruct#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): FooStruct#})[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<FooStruct>.Type#]; name=self
// RESOLVE_GENERIC_PARAMS_1_STATIC-DAG: Keyword/CurrNominal:              .Type[#FooGenericStruct<FooStruct>.Type#]; name=Type
}

func testResolveGenericParams2<Foo : FooProtocol>(_ foo: Foo) {
  FooGenericStruct<Foo>()#^RESOLVE_GENERIC_PARAMS_2^#
// RESOLVE_GENERIC_PARAMS_2-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#FooProtocol#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[FooProtocol]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): FooProtocol#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): FooProtocol#})[#FooProtocol#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<Foo>#]; name=self

  FooGenericStruct<Foo>#^RESOLVE_GENERIC_PARAMS_2_STATIC^#
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooGenericStruct<FooProtocol>#]; name=()
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#t: FooProtocol#})[#FooGenericStruct<FooProtocol>#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(self): &FooGenericStruct<FooProtocol>#})[#(FooProtocol) -> Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(self): &FooGenericStruct<FooProtocol>#})[#(FooProtocol) -> FooProtocol#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(self): &FooGenericStruct<FooProtocol>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): FooProtocol#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): FooProtocol#})[#FooProtocol#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<Foo>.Type#]; name=self
// RESOLVE_GENERIC_PARAMS_2_STATIC-DAG: Keyword/CurrNominal:              .Type[#FooGenericStruct<Foo>.Type#]; name=Type

}

struct TestResolveGenericParams3_4<T> {
  func testResolveGenericParams3() {
    FooGenericStruct<FooStruct>()#^RESOLVE_GENERIC_PARAMS_3^#
// RESOLVE_GENERIC_PARAMS_3-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[FooStruct]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): FooStruct#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): FooStruct#})[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<FooStruct>#]; name=self

    FooGenericStruct<FooStruct>#^RESOLVE_GENERIC_PARAMS_3_STATIC^#
// RESOLVE_GENERIC_PARAMS_3_STATIC: Begin completions, 12 items
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooGenericStruct<FooStruct>#]; name=()
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#t: FooStruct#})[#FooGenericStruct<FooStruct>#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(FooStruct) -> Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(FooStruct) -> FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(self): &FooGenericStruct<FooStruct>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): FooStruct#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): FooStruct#})[#FooStruct#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<FooStruct>.Type#]; name=self
// RESOLVE_GENERIC_PARAMS_3_STATIC-DAG: Keyword/CurrNominal:              .Type[#FooGenericStruct<FooStruct>.Type#]; name=Type
  }

  func testResolveGenericParams4(_ t: T) {
    FooGenericStruct<T>(t: t)#^RESOLVE_GENERIC_PARAMS_4^#
// RESOLVE_GENERIC_PARAMS_4-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#T#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[T]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): T#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): T#})[#T#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<T>#]; name=self

    FooGenericStruct<T>#^RESOLVE_GENERIC_PARAMS_4_STATIC^#
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooGenericStruct<T>#]; name=()
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#t: T#})[#FooGenericStruct<T>#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(self): &FooGenericStruct<T>#})[#(T) -> Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(self): &FooGenericStruct<T>#})[#(T) -> T#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(self): &FooGenericStruct<T>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): T#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): T#})[#T#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<T>.Type#]; name=self
// RESOLVE_GENERIC_PARAMS_4_STATIC-DAG: Keyword/CurrNominal:              .Type[#FooGenericStruct<T>.Type#]; name=Type
  }

  func testResolveGenericParams5<U>(_ u: U) {
    FooGenericStruct<U>(t: u)#^RESOLVE_GENERIC_PARAMS_5^#
// RESOLVE_GENERIC_PARAMS_5-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5-DAG: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[U]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): U#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<U>#]; name=self

    FooGenericStruct<U>#^RESOLVE_GENERIC_PARAMS_5_STATIC^#
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ()[#FooGenericStruct<U>#]; name=()
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:    ({#t: U#})[#FooGenericStruct<U>#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(self): &FooGenericStruct<U>#})[#(U) -> Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(self): &FooGenericStruct<U>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(self): &FooGenericStruct<U>#})[#(U) -> U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): U#})[#Void#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{; name=.+$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Keyword[self]/CurrNominal:        .self[#FooGenericStruct<U>.Type#]; name=self
// RESOLVE_GENERIC_PARAMS_5_STATIC-DAG: Keyword/CurrNominal:              .Type[#FooGenericStruct<U>.Type#]; name=Type
  }
}

func testResolveGenericParamsError1() {
  // There is no type 'Foo'.  Check that we don't crash.
  // FIXME: we could also display correct completion results here, because
  // swift does not have specialization, and the set of completion results does
  // not depend on the generic type argument.
  FooGenericStruct<NotDefinedType>()#^RESOLVE_GENERIC_PARAMS_ERROR_1?check=NOCRASH^#
}

//===--- Check that we can code complete expressions that have unsolved type variables.

class BuilderStyle<T> {
  var count = 0
  func addString(_ s: String) -> BuilderStyle<T> {
    count += 1
    return self
  }
  func add(_ t: T) -> BuilderStyle<T> {
    count += 1
    return self
  }
  func get() -> Int {
    return count
  }
}

func testTypeCheckWithUnsolvedVariables1() {
  BuilderStyle().#^TC_UNSOLVED_VARIABLES_1^#
}
// TC_UNSOLVED_VARIABLES_1-DAG: Keyword[self]/CurrNominal: self[#BuilderStyle<_>#]; name=self
// TC_UNSOLVED_VARIABLES_1-DAG: Decl[InstanceVar]/CurrNominal: count[#Int#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_1-DAG: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<_>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_1-DAG: Decl[InstanceMethod]/CurrNominal: add({#(t): _#})[#BuilderStyle<_>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_1-DAG: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{; name=.+$}}

func testTypeCheckWithUnsolvedVariables2() {
  BuilderStyle().addString("abc").#^TC_UNSOLVED_VARIABLES_2^#
}
// TC_UNSOLVED_VARIABLES_2-DAG: Keyword[self]/CurrNominal: self[#BuilderStyle<_>#]; name=self
// TC_UNSOLVED_VARIABLES_2-DAG: Decl[InstanceVar]/CurrNominal:    count[#Int#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_2-DAG: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<_>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_2-DAG: Decl[InstanceMethod]/CurrNominal: add({#(t): _#})[#BuilderStyle<_>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_2-DAG: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{; name=.+$}}

func testTypeCheckWithUnsolvedVariables3() {
  BuilderStyle().addString("abc").add(42).#^TC_UNSOLVED_VARIABLES_3^#
}
// TC_UNSOLVED_VARIABLES_3-DAG: Keyword[self]/CurrNominal: self[#BuilderStyle<Int>#]; name=self
// TC_UNSOLVED_VARIABLES_3-DAG: Decl[InstanceVar]/CurrNominal:    count[#Int#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_3-DAG: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<Int>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_3-DAG: Decl[InstanceMethod]/CurrNominal: add({#(t): Int#})[#BuilderStyle<Int>#]{{; name=.+$}}
// TC_UNSOLVED_VARIABLES_3-DAG: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{; name=.+$}}

func testTypeCheckNil() {
  nil#^TC_UNSOLVED_VARIABLES_4^#
}
// TC_UNSOLVED_VARIABLES_4-NOT: Decl{{.*}}: .{{[a-zA-Z]}}

//===--- Check that we can look up into modules

func testResolveModules1() {
  Swift#^RESOLVE_MODULES_1^#
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: .Int8[#Int8#]{{; name=.+$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: .Int16[#Int16#]{{; name=.+$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: .Int32[#Int32#]{{; name=.+$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: .Int64[#Int64#]{{; name=.+$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: .Bool[#Bool#]{{; name=.+$}}
// RESOLVE_MODULES_1-DAG: Decl[TypeAlias]/OtherModule[Swift]/IsSystem: .Float32[#Float#]{{; name=.+$}}
}

//===--- Check that we can complete inside interpolated string literals

func testInterpolatedString1() {
  "\(fooObject.#^INTERPOLATED_STRING_1^#)"
}

// INTERPOLATED_STRING_1-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]:      lazyInstanceVar[#Int#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]:      instanceVar[#Int#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: instanceFunc0()[#Void#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: instanceFunc1({#(a): Int#})[#Void#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: instanceFunc2({#(a): Int#}, {#b: &Double#})[#Void#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: instanceFunc3({#(a): Int#}, {#(Float, Double)#})[#Void#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: instanceFunc4({#(a): Int?#}, {#b: Int!#}, {#c: &Int?#}, {#d: &Int!#})[#Void#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal:   instanceFunc5()[#Int?#]{{; name=.+$}}
// INTERPOLATED_STRING_1-DAG: Decl[InstanceMethod]/CurrNominal:   instanceFunc6()[#Int!#]{{; name=.+$}}

//===--- Check protocol extensions

struct WillConformP1 {
}

protocol P1 {
  func reqP1()
}

protocol P2 : P1 {
  func reqP2()
}

protocol P3 : P1, P2 {
}

extension P1 {
  final func extP1() {}
}

extension P2 {
  final func extP2() {}
}

extension P3 {
  final func reqP1() {}
  final func reqP2() {}
  final func extP3() {}
}

extension WillConformP1 : P1 {
  func reqP1() {}
}

struct DidConformP2 : P2 {
  func reqP1() {}
  func reqP2() {}
}

struct DidConformP3 : P3 {
}

func testProtocol1(_ x: P1) {
  x.#^PROTOCOL_EXT_P1^#
}
// PROTOCOL_EXT_P1-DAG: Decl[InstanceMethod]/CurrNominal:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P1-DAG: Decl[InstanceMethod]/CurrNominal:   extP1()[#Void#]{{; name=.+$}}


func testProtocol2(_ x: P2) {
  x.#^PROTOCOL_EXT_P2^#
}
// PROTOCOL_EXT_P2-DAG: Decl[InstanceMethod]/Super:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P2-DAG: Decl[InstanceMethod]/Super:   extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P2-DAG: Decl[InstanceMethod]/CurrNominal:   reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P2-DAG: Decl[InstanceMethod]/CurrNominal:   extP2()[#Void#]{{; name=.+$}}

func testProtocol3(_ x: P3) {
  x.#^PROTOCOL_EXT_P3^#
}

// PROTOCOL_EXT_P3-DAG: Decl[InstanceMethod]/CurrNominal:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P3-DAG: Decl[InstanceMethod]/CurrNominal:   reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P3-DAG: Decl[InstanceMethod]/CurrNominal:   extP3()[#Void#]{{; name=.+$}}

// PROTOCOL_EXT_P3-DAG: Decl[InstanceMethod]/Super:   extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P3-DAG: Decl[InstanceMethod]/Super:   extP2()[#Void#]{{; name=.+$}}

func testConformingConcrete1(_ x: WillConformP1) {
  x.#^PROTOCOL_EXT_WILLCONFORMP1^#
}
// PROTOCOL_EXT_WILLCONFORMP1-DAG: Decl[InstanceMethod]/CurrNominal:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_WILLCONFORMP1-DAG: Decl[InstanceMethod]/Super:   extP1()[#Void#]{{; name=.+$}}

func testConformingConcrete2(_ x: DidConformP2) {
  x.#^PROTOCOL_EXT_DIDCONFORMP2^#
}
// PROTOCOL_EXT_DIDCONFORMP2-DAG: Decl[InstanceMethod]/CurrNominal:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP2-DAG: Decl[InstanceMethod]/CurrNominal:   reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP2-DAG: Decl[InstanceMethod]/Super:   extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP2-DAG: Decl[InstanceMethod]/Super:   extP2()[#Void#]{{; name=.+$}}

func testConformingConcrete3(_ x: DidConformP3) {
  x.#^PROTOCOL_EXT_DIDCONFORMP3^#
}
// FIXME: the next two should both be "CurrentNominal"
// PROTOCOL_EXT_DIDCONFORMP3-DAG: Decl[InstanceMethod]/Super:   reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP3-DAG: Decl[InstanceMethod]/Super:   reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP3-DAG: Decl[InstanceMethod]/Super:   extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP3-DAG: Decl[InstanceMethod]/Super:   extP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_DIDCONFORMP3-DAG: Decl[InstanceMethod]/Super:   extP3()[#Void#]{{; name=.+$}}

func testGenericConforming1<T: P1>(x: T) {
  x.#^PROTOCOL_EXT_GENERICP1^#
}
// PROTOCOL_EXT_GENERICP1-DAG: Decl[InstanceMethod]/CurrNominal: reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP1-DAG: Decl[InstanceMethod]/CurrNominal: extP1()[#Void#]{{; name=.+$}}

func testGenericConforming2<T: P2>(x: T) {
  x.#^PROTOCOL_EXT_GENERICP2^#
}
// PROTOCOL_EXT_GENERICP2-DAG: Decl[InstanceMethod]/Super: reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP2-DAG: Decl[InstanceMethod]/CurrNominal:  reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP2-DAG: Decl[InstanceMethod]/Super: extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP2-DAG: Decl[InstanceMethod]/CurrNominal: extP2()[#Void#]{{; name=.+$}}

func testGenericConforming3<T: P3>(x: T) {
  x.#^PROTOCOL_EXT_GENERICP3^#
}
// PROTOCOL_EXT_GENERICP3-DAG: Decl[InstanceMethod]/CurrNominal: reqP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP3-DAG: Decl[InstanceMethod]/CurrNominal: reqP2()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP3-DAG: Decl[InstanceMethod]/CurrNominal: extP3()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP3-DAG: Decl[InstanceMethod]/Super: extP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_GENERICP3-DAG: Decl[InstanceMethod]/Super: extP2()[#Void#]{{; name=.+$}}

protocol NoDupReq1 {
  func foo()
  func roo(arg1: Int)
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}
protocol NoDupReq2 {
  func foo()
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}
protocol NoDupReq3 {
  func foo()
  func roo(arg2: Int)
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}

protocol NoDupReq4 {
  func foo()
  func roo(arg1: Int)
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}
protocol NoDupReq5: NoDupReq4 {
  func foo()
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}
protocol NoDupReq6: NoDupReq5 {
  func foo()
  func roo(arg2: Int)
  subscript(arg: Bool) -> Bool {get}
  var doo: Int {get}
  associatedtype E
}

typealias NoDupReq23 = NoDupReq2 & NoDupReq3

protocol Override {
  func foo<T: NoDupReq1>(_ arg: T)
  func foo<T: NoDupReq2>(_ arg: T)
}
protocol Override2 {
  func foo<T: NoDupReq1>(_ arg: T)
}
protocol Override3: Override2 {
  func foo<T: NoDupReq2>(_ arg: T)
}

func checkRestatementNoDup1(_ arg: NoDupReq1 & NoDupReq2 & NoDupReq3) {
  arg.#^NODUP_RESTATED_REQ1?check=CHECK_NODUP_RESTATED_REQ^#
  arg#^NODUP_RESTATED_REQ_NODOT1?check=CHECK_NODUP_RESTATED_REQ_NODOT^#
}
func checkRestatementNoDup2(_ arg: NoDupReq6) {
  arg.#^NODUP_RESTATED_REQ2?check=CHECK_NODUP_RESTATED_REQ^#
}
func checkRestatementNoDup3<T: NoDupReq6>(_ arg: T) {
  arg.#^NODUP_RESTATED_REQ3?check=CHECK_NODUP_RESTATED_REQ^#
  T.#^NODUP_RESTATED_REQ_TYPE1?check=CHECK_NODUP_RESTATED_REQ_TYPE1^#
  arg#^NODUP_RESTATED_REQ_NODOT2?check=CHECK_NODUP_RESTATED_REQ_NODOT^#
}
func checkRestatementNoDup4<T: NoDupReq1 & NoDupReq2 & NoDupReq3>(_ arg: T) {
  arg.#^NODUP_RESTATED_REQ4?check=CHECK_NODUP_RESTATED_REQ^#
  T.#^NODUP_RESTATED_REQ_TYPE2?check=CHECK_NODUP_RESTATED_REQ_TYPE2^#
}
func checkRestatementNoDup5<T: NoDupReq1 & NoDupReq23>(_ arg: T) {
  arg.#^NODUP_RESTATED_REQ5?check=CHECK_NODUP_RESTATED_REQ^#
  T.#^NODUP_RESTATED_REQ_TYPE3?check=CHECK_NODUP_RESTATED_REQ_TYPE3^#
}
func checkRestatementNoDup6(_ arg: NoDupReq1 & NoDupReq23) {
  arg.#^NODUP_RESTATED_REQ6?check=CHECK_NODUP_RESTATED_REQ^#
  arg#^NODUP_RESTATED_REQ_NODOT3?check=CHECK_NODUP_RESTATED_REQ_NODOT^#
}
func checkOverrideInclusion1(_ arg: Override) {
  arg.#^CHECK_PROT_OVERRIDES1?check=CHECK_PROT_OVERRIDES^#
}
func checkOverrideInclusion2(_ arg: Override3) {
  arg.#^CHECK_PROT_OVERRIDES2?check=CHECK_PROT_OVERRIDES^#
}

// CHECK_NODUP_RESTATED_REQ-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: foo()[#Void#]; name=foo()
// CHECK_NODUP_RESTATED_REQ-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: roo({#arg1: Int#})[#Void#]
// CHECK_NODUP_RESTATED_REQ-DAG: Decl[InstanceVar]/{{Super|CurrNominal}}:    doo[#Int#]; name=doo
// CHECK_NODUP_RESTATED_REQ-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: roo({#arg2: Int#})[#Void#]
// CHECK_NODUP_RESTATED_REQ-NOT: Decl[InstanceMethod]/{{Super|CurrNominal}}: foo()[#Void#]; name=foo()
// CHECK_NODUP_RESTATED_REQ-NOT: Decl[InstanceVar]/{{Super|CurrNominal}}:    doo[#Int#]; name=doo

// CHECK_NODUP_RESTATED_REQ_NODOT: Begin completions, 6 items
// CHECK_NODUP_RESTATED_REQ_NODOT-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: .foo()[#Void#]; name=foo()
// CHECK_NODUP_RESTATED_REQ_NODOT-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: .roo({#arg1: Int#})[#Void#];
// CHECK_NODUP_RESTATED_REQ_NODOT-DAG: Decl[Subscript]/{{Super|CurrNominal}}:      [{#(arg): Bool#}][#Bool#]; name=[:]
// CHECK_NODUP_RESTATED_REQ_NODOT-DAG: Decl[InstanceVar]/{{Super|CurrNominal}}:    .doo[#Int#]; name=doo
// CHECK_NODUP_RESTATED_REQ_NODOT-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: .roo({#arg2: Int#})[#Void#];

// CHECK_NODUP_RESTATED_REQ_TYPE1: Begin completions, 6 items
// CHECK_NODUP_RESTATED_REQ_TYPE1: Decl[InstanceMethod]/CurrNominal: foo({#(self): NoDupReq6#})[#() -> Void#]; name=foo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE1: Decl[InstanceMethod]/CurrNominal: roo({#(self): NoDupReq6#})[#(arg2: Int) -> Void#]; name=roo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE1: Decl[AssociatedType]/CurrNominal: E; name=E
// CHECK_NODUP_RESTATED_REQ_TYPE1: Decl[InstanceMethod]/Super: roo({#(self): NoDupReq6#})[#(arg1: Int) -> Void#]; name=roo(:)

// CHECK_NODUP_RESTATED_REQ_TYPE2: Begin completions, 6 items
// CHECK_NODUP_RESTATED_REQ_TYPE2: Decl[InstanceMethod]/CurrNominal: foo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#() -> Void#]; name=foo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE2: Decl[InstanceMethod]/CurrNominal: roo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#(arg1: Int) -> Void#]; name=roo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE2: Decl[AssociatedType]/CurrNominal: E; name=E
// CHECK_NODUP_RESTATED_REQ_TYPE2: Decl[InstanceMethod]/CurrNominal: roo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#(arg2: Int) -> Void#]; name=roo(:)

// CHECK_NODUP_RESTATED_REQ_TYPE3: Begin completions, 6 items
// CHECK_NODUP_RESTATED_REQ_TYPE3: Decl[InstanceMethod]/CurrNominal: foo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#() -> Void#]; name=foo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE3: Decl[InstanceMethod]/CurrNominal: roo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#(arg1: Int) -> Void#]; name=roo(:)
// CHECK_NODUP_RESTATED_REQ_TYPE3: Decl[AssociatedType]/CurrNominal: E; name=E
// CHECK_NODUP_RESTATED_REQ_TYPE3: Decl[InstanceMethod]/CurrNominal: roo({#(self): NoDupReq1 & NoDupReq2 & NoDupReq3#})[#(arg2: Int) -> Void#]; name=roo(:)

// CHECK_PROT_OVERRIDES-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: foo({#(arg): NoDupReq1#})[#Void#]; name=foo(:)
// CHECK_PROT_OVERRIDES-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: foo({#(arg): NoDupReq2#})[#Void#]; name=foo(:)

struct OnlyMe {}
protocol P4 {
  associatedtype T
}
extension P4 where Self.T : P1 {
  final func extP4WhenP1() {}
  final var x: Int { return 1 }
  init() {}
}
extension P4 where Self.T : P1 {
  init(x: Int) {}
}
extension P4 where Self.T == OnlyMe {
  final func extP4OnlyMe() {}
  final subscript(x: Int) -> Int { return 2 }
}
struct Concrete1 : P4 {
  typealias T = WillConformP1
}
struct Generic1<S: P1> : P4 {
  typealias T = S
}
struct Concrete2 : P4 {
  typealias T = OnlyMe
}
struct Generic2<S> : P4 {
  typealias T = S
}

func testConstrainedP4(_ x: P4) {
  x.#^PROTOCOL_EXT_P4^#
}
// PROTOCOL_EXT_P4-NOT: extP4

func testConstrainedConcrete1(_ x: Concrete1) {
  x.#^PROTOCOL_EXT_CONCRETE1?check=PROTOCOL_EXT_P4_P1^#
}
func testConstrainedConcrete2(_ x: Generic1<WillConformP1>) {
  x.#^PROTOCOL_EXT_CONCRETE2?check=PROTOCOL_EXT_P4_P1^#
}
func testConstrainedGeneric1<S: P1>(x: Generic1<S>) {
  x.#^PROTOCOL_EXT_CONSTRAINED_GENERIC_1?check=PROTOCOL_EXT_P4_P1^#
}
func testConstrainedGeneric2<S: P4 where S.T : P1>(x: S) {
  x.#^PROTOCOL_EXT_CONSTRAINED_GENERIC_2?check=PROTOCOL_EXT_P4_P1^#
}
extension Concrete1 {
  func testInsideConstrainedConcrete1_1() {
    #^PROTOCOL_EXT_INSIDE_CONCRETE1_1?check=PROTOCOL_EXT_P4_P1^#
  }
  func testInsideConstrainedConcrete1_2() {
    self.#^PROTOCOL_EXT_INSIDE_CONCRETE1_2?check=PROTOCOL_EXT_P4_P1^#
  }
}
// PROTOCOL_EXT_P4_P1-NOT: extP4OnlyMe()
// PROTOCOL_EXT_P4_P1-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: extP4WhenP1()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P4_P1-DAG: Decl[InstanceVar]/{{Super|CurrNominal}}: x[#Int#]{{; name=.+$}}
// PROTOCOL_EXT_P4_P1-NOT: extP4OnlyMe()

func testConstrainedConcrete3(_ x: Concrete2) {
  x.#^PROTOCOL_EXT_CONCRETE3?check=PROTOCOL_EXT_P4_ONLYME^#
}
func testConstrainedConcrete3_sub(_ x: Concrete2) {
  x#^PROTOCOL_EXT_CONCRETE3_SUB?check=PROTOCOL_EXT_P4_ONLYME_SUB^#
}
func testConstrainedConcrete4(_ x: Generic2<OnlyMe>) {
  x.#^PROTOCOL_EXT_CONCRETE4?check=PROTOCOL_EXT_P4_ONLYME^#
}
func testConstrainedGeneric1<S: P4 where S.T == OnlyMe>(x: S) {
  x.#^PROTOCOL_EXT_CONSTRAINED_GENERIC_3?check=PROTOCOL_EXT_P4_ONLYME^#
}
func testConstrainedGeneric1_sub<S: P4 where S.T == OnlyMe>(x: S) {
  x#^PROTOCOL_EXT_CONSTRAINED_GENERIC_3_SUB?check=PROTOCOL_EXT_P4_ONLYME_SUB^#
}
extension Concrete2 {
  func testInsideConstrainedConcrete2_1() {
    #^PROTOCOL_EXT_INSIDE_CONCRETE2_1?check=PROTOCOL_EXT_P4_ONLYME^#
  }
  func testInsideConstrainedConcrete2_2() {
    self.#^PROTOCOL_EXT_INSIDE_CONCRETE2_2?check=PROTOCOL_EXT_P4_ONLYME^#
  }
}
// PROTOCOL_EXT_P4_ONLYME-NOT: extP4WhenP1()
// PROTOCOL_EXT_P4_ONLYME-NOT: x[#Int#]
// PROTOCOL_EXT_P4_ONLYME-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: extP4OnlyMe()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P4_ONLYME-NOT: extP4WhenP1()
// PROTOCOL_EXT_P4_ONLYME-NOT: x[#Int#]

// PROTOCOL_EXT_P4_ONLYME_SUB: Decl[Subscript]/{{Super|CurrNominal}}: [{#(x): Int#}][#Int#]{{; name=.+$}}

func testTypealias1() {
  Concrete1.#^PROTOCOL_EXT_TA_1^#
}
// PROTOCOL_EXT_TA_1-DAG: Decl[TypeAlias]/CurrNominal: T
func testTypealias1<S: P4 where S.T == WillConformP1>() {
  S.#^PROTOCOL_EXT_TA_2^#
}
// PROTOCOL_EXT_TA_2-DAG: Decl[AssociatedType]/CurrNominal: T

func testProtExtInit1() {
  Concrete1(#^PROTOCOL_EXT_INIT_1^#
}

// PROTOCOL_EXT_INIT_1: Decl[Constructor]/{{Super|CurrNominal}}/Flair[ArgLabels]: ['(']{#x: Int#}[')'][#Concrete1#]{{; name=.+$}}

func testProtExtInit2<S: P4 where S.T : P1>() {
  do {
  S(#^PROTOCOL_EXT_INIT_2^#
  }
  S.#^PROTOCOL_EXT_INIT_3^#
  S#^PROTOCOL_EXT_INIT_4^#

  var sTy: S.Type = S.self
  sTy.init(#^PROTOCOL_EXT_INIT_5^#
}

// PROTOCOL_EXT_INIT_2: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#x: Int#}[')'][#P4#]{{; name=.+$}}
// PROTOCOL_EXT_INIT_3: Decl[Constructor]/CurrNominal: init({#x: Int#})[#P4#]{{; name=.+$}}
// PROTOCOL_EXT_INIT_4: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#x: Int#})[#P4#]{{; name=.+$}}
// PROTOCOL_EXT_INIT_5: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#x: Int#}[')'][#P4#]{{; name=.+$}}

extension P4 where Self.T == OnlyMe {
  final func test1() {
    self.#^PROTOCOL_EXT_P4_DOT_1?check=PROTOCOL_EXT_P4_DOT^#
  }
  final func test2() {
    #^PROTOCOL_EXT_P4_DOT_2?check=PROTOCOL_EXT_P4_DOT^#
  }
}
// PROTOCOL_EXT_P4_DOT-NOT: extP4WhenP1()
// PROTOCOL_EXT_P4_DOT-DAG: Decl[InstanceMethod]/{{Super|CurrNominal}}: extP4OnlyMe()[#Void#]{{; name=.+$}}
// PROTOCOL_EXT_P4_DOT-NOT: extP4WhenP1()

extension P4 where Self.T == WillConformP1 {
  final func test() {
    T.#^PROTOCOL_EXT_P4_T_DOT_1^#
  }
}
// PROTOCOL_EXT_P4_T_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   reqP1({#(self): WillConformP1#})[#() -> Void#]{{; name=.+$}}
// PROTOCOL_EXT_P4_T_DOT_1-DAG: Decl[InstanceMethod]/Super:   extP1({#(self): WillConformP1#})[#() -> Void#]{{; name=.+$}}

protocol PWithT {
  associatedtype T
  func foo(_ x: T) -> T
}

extension PWithT {
  final func bar(_ x: T) -> T {
    return x
  }
}

func testUnusableProtExt(_ x: PWithT) {
  x.#^PROTOCOL_EXT_UNUSABLE_EXISTENTIAL^#
}
// FIXME(https://github.com/apple/swift/issues/65696): We should not be showing these because (1) they cannot be accessed on the existential (2) we don't have the syntax and features to represent the projected type sigs anyway.
// PROTOCOL_EXT_UNUSABLE_EXISTENTIAL: Decl[InstanceMethod]/CurrNominal:   foo({#(x): any PWithT.T#})[#any PWithT.T#]{{; name=.+}}
// PROTOCOL_EXT_UNUSABLE_EXISTENTIAL: Decl[InstanceMethod]/CurrNominal:   bar({#(x): any PWithT.T#})[#any PWithT.T#]{{; name=.+}}

protocol dedupP {
  associatedtype T
  func foo() -> T
  var bar: T {get}
  subscript(x: T) -> T {get}
}

extension dedupP {
  func foo() -> T { return T() }
  var bar: T { return T() }
  subscript(x: T) -> T { return T() }
}

struct dedupS : dedupP {
  func foo() -> Int { return T() }
  var bar: Int = 5
  subscript(x: Int) -> Int { return 10 }
}

func testDeDuped(_ x: dedupS) {
  x#^PROTOCOL_EXT_DEDUP_1^#
// FIXME: Should produce 3 items (?)

// PROTOCOL_EXT_DEDUP_1: Begin completions, 5 items
// PROTOCOL_EXT_DEDUP_1-DAG: Decl[InstanceMethod]/CurrNominal:   .foo()[#Int#]; name=foo()
// PROTOCOL_EXT_DEDUP_1-DAG: Decl[InstanceVar]/CurrNominal:      .bar[#Int#]; name=bar
// PROTOCOL_EXT_DEDUP_1-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}][#Int#]; name=[:]
// PROTOCOL_EXT_DEDUP_1-DAG: Decl[InstanceVar]/Super:            .bar[#Int#]; name=bar
// PROTOCOL_EXT_DEDUP_1-DAG: Keyword[self]/CurrNominal:          .self[#dedupS#]; name=self
}
func testDeDuped2(_ x: dedupP) {
  x#^PROTOCOL_EXT_DEDUP_2^#
// PROTOCOL_EXT_DEDUP_2: Begin completions, 5 items
// PROTOCOL_EXT_DEDUP_2-DAG: Decl[InstanceMethod]/CurrNominal:   .foo()[#any dedupP.T#]; name=foo()
// PROTOCOL_EXT_DEDUP_2-DAG: Decl[InstanceVar]/CurrNominal:      .bar[#any dedupP.T#]; name=bar
// FIXME(https://github.com/apple/swift/issues/65696): We should not be showing this because (1) it cannot be accessed on the existential (2) we don't have the syntax and features to represent the projected type sig anyway.
// PROTOCOL_EXT_DEDUP_2-DAG: Decl[Subscript]/CurrNominal:        [{#(x): any dedupP.T#}][#any dedupP.T#]; name=[:]
// PROTOCOL_EXT_DEDUP_2-DAG: Keyword[self]/CurrNominal:          .self[#any dedupP#]; name=self
}
func testDeDuped3<T : dedupP where T.T == Int>(_ x: T) {
  x#^PROTOCOL_EXT_DEDUP_3^#
// PROTOCOL_EXT_DEDUP_3: Begin completions, 5 items
// PROTOCOL_EXT_DEDUP_3-DAG: Decl[InstanceMethod]/CurrNominal:   .foo()[#Int#]; name=foo()
// PROTOCOL_EXT_DEDUP_3-DAG: Decl[InstanceVar]/CurrNominal:      .bar[#Int#]; name=bar
// PROTOCOL_EXT_DEDUP_3-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}][#Int#]; name=[:]
// PROTOCOL_EXT_DEDUP_3-DAG: Keyword[self]/CurrNominal:          .self[#T#]; name=self
}

//===--- Check calls that may throw

func globalFuncThrows() throws {}
func globalFuncRethrows(_ x: () throws -> ()) rethrows {}
struct HasThrowingMembers {
  func memberThrows() throws {}
  func memberRethrows(_ x: () throws -> ()) rethrows {}
  init() throws {}
  init(x: () throws -> ()) rethrows {}
}

func testThrows001() {
  globalFuncThrows#^THROWS1^#

// THROWS1: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ()[' throws'][#Void#]; name=()
}
func testThrows002() {
  globalFuncRethrows#^THROWS2^#

// THROWS2: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ({#(x): () throws -> ()##() throws -> ()#})[' rethrows'][#Void#]; name=(:)
}
func testThrows003(_ x: HasThrowingMembers) {
  x.#^MEMBER_THROWS1^#
// MEMBER_THROWS1-DAG: Decl[InstanceMethod]/CurrNominal:   memberThrows()[' throws'][#Void#]
// MEMBER_THROWS1-DAG: Decl[InstanceMethod]/CurrNominal:   memberRethrows {|}[' rethrows'][#Void#]
// MEMBER_THROWS1-DAG: Decl[InstanceMethod]/CurrNominal:   memberRethrows({#(x): () throws -> ()##() throws -> ()#})[' rethrows'][#Void#]
}
func testThrows004(_ x: HasThrowingMembers) {
  x.memberThrows#^MEMBER_THROWS2^#
// MEMBER_THROWS2: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ()[' throws'][#Void#]; name=()
}
func testThrows005(_ x: HasThrowingMembers) {
  x.memberRethrows#^MEMBER_THROWS3^#
// MEMBER_THROWS3: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(x): () throws -> ()##() throws -> ()#})[' rethrows'][#Void#]; name=(:)
}
func testThrows006() {
  HasThrowingMembers(#^INIT_THROWS1^#
// INIT_THROWS1: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#x: () throws -> ()##() throws -> ()#}[')'][' rethrows'][#HasThrowingMembers#]
}


// rdar://21346928
// Just sample some String API to sanity check.
// AUTOCLOSURE_STRING: Decl[InstanceVar]/CurrNominal{{.*}}:      {{.*}}unicodeScalars[#String.UnicodeScalarView#]
// AUTOCLOSURE_STRING: Decl[InstanceVar]/CurrNominal{{.*}}:      {{.*}}utf16[#String.UTF16View#]
func testWithAutoClosure1(_ x: String?) {
  (x ?? "autoclosure").#^AUTOCLOSURE1?check=AUTOCLOSURE_STRING^#
}
func testWithAutoClosure2(_ x: String?) {
  let y = (x ?? "autoclosure").#^AUTOCLOSURE2?check=AUTOCLOSURE_STRING^#
}
func testWithAutoClosure3(_ x: String?) {
  let y = (x ?? "autoclosure".#^AUTOCLOSURE3?check=AUTOCLOSURE_STRING^#)
}
func testWithAutoClosure4(_ x: String?) {
  let y = { let z = (x ?? "autoclosure").#^AUTOCLOSURE4?check=AUTOCLOSURE_STRING^# }
}
func testWithAutoClosure5(_ x: String?) {
  if let y = (x ?? "autoclosure").#^AUTOCLOSURE5?check=AUTOCLOSURE_STRING^# {
  }
}

func testGenericTypealias1() {
  typealias MyPair<T> = (T, T)
  var x: MyPair<Int>
  x.#^GENERIC_TYPEALIAS_1^#
}
// GENERIC_TYPEALIAS_1: Pattern/CurrNominal:                0[#Int#];
// GENERIC_TYPEALIAS_1: Pattern/CurrNominal:                1[#Int#];

func testGenericTypealias2() {
  struct Enclose {
    typealias MyPair<T> = (T, T)
  }
  Enclose.#^GENERIC_TYPEALIAS_2^#
}
// GENERIC_TYPEALIAS_2: Decl[TypeAlias]/CurrNominal:        MyPair[#(T, T)#];

struct Deprecated {
  @available(*, deprecated)
  func deprecated(x: Deprecated) {
    x.#^DEPRECATED_1^#
  }
}
// DEPRECATED_1: Decl[InstanceMethod]/CurrNominal/NotRecommended: deprecated({#x: Deprecated#})[#Void#];

struct Person {
  var firstName: String
}
class Other { var nameFromOther: Int = 1 }
class TestDotExprWithNonNominal {
  var otherField: Other

  func test1() {
    let person = Person(firstName: otherField.#^DOT_EXPR_NON_NOMINAL_1^#)
// DOT_EXPR_NON_NOMINAL_1-NOT: Instance
// DOT_EXPR_NON_NOMINAL_1: Keyword[self]/CurrNominal:          self[#Other#]; name=self
// DOT_EXPR_NON_NOMINAL_1: Decl[InstanceVar]/CurrNominal:      nameFromOther[#Int#];
// DOT_EXPR_NON_NOMINAL_1-NOT: Instance
  }
  func test2() {
    let person = Person(firstName: 1.#^DOT_EXPR_NON_NOMINAL_2^#)
// DOT_EXPR_NON_NOMINAL_2-NOT: otherField
// DOT_EXPR_NON_NOMINAL_2-NOT: firstName
// DOT_EXPR_NON_NOMINAL_2: Keyword[self]/CurrNominal:          self[#Int#]; name=self
// DOT_EXPR_NON_NOMINAL_2: Decl[InstanceVar]/CurrNominal/IsSystem: hashValue[#Int#];
// DOT_EXPR_NON_NOMINAL_2-NOT: otherField
// DOT_EXPR_NON_NOMINAL_2-NOT: firstName
  }
}

class Cat {
  struct Inner {
    var prop1: String
    var prop2: String
  }
  var `class`: Inner
}
func testKeyword(cat: Cat) {
  let _ = cat.#^KEYWORD_1^#
// KEYWORD_1-DAG: Keyword[self]/CurrNominal:          self[#Cat#]; name=self
// KEYWORD_1-DAG: Decl[InstanceVar]/CurrNominal:      class[#Cat.Inner#]; name=class

  let _ = cat.class#^KEYWORD_2^#
// KEYWORD_2-DAG: Decl[InstanceVar]/CurrNominal:      .prop1[#String#]; name=prop1
// KEYWORD_2-DAG: Decl[InstanceVar]/CurrNominal:      .prop2[#String#]; name=prop2
// KEYWORD_2-DAG: BuiltinOperator/None:                = {#Cat.Inner#}; name==

  let _ = cat.class.#^KEYWORD_3^#
// KEYWORD_3-DAG: Decl[InstanceVar]/CurrNominal:      prop1[#String#]; name=prop1
// KEYWORD_3-DAG: Decl[InstanceVar]/CurrNominal:      prop2[#String#]; name=prop2
}

protocol ExistentialProto {
  static func staticMethod()
  func instanceMethod()
}

func testExistential() {
  let _ = ExistentialProto.#^PROTOCOLTYPE_DOT_1^#
// PROTOCOLTYPE_DOT_1: Begin completions, 3 items
// PROTOCOLTYPE_DOT_1-DAG: Keyword[self]/CurrNominal:          self[#(any ExistentialProto).Type#]; name=self
// PROTOCOLTYPE_DOT_1-DAG: Keyword/CurrNominal:                Protocol[#(any ExistentialProto).Type#]; name=Protocol
// PROTOCOLTYPE_DOT_1-DAG: Keyword/CurrNominal:                Type[#any ExistentialProto.Type#]; name=Type

  let _ = ExistentialProto.Type.#^PROTOCOLTYPE_DOT_2^#
// PROTOCOLTYPE_DOT_2: Begin completions, 3 items
// PROTOCOLTYPE_DOT_2-DAG: Keyword[self]/CurrNominal:          self[#(any ExistentialProto.Type).Type#]; name=self
// PROTOCOLTYPE_DOT_2-DAG: Keyword/CurrNominal:                Protocol[#(any ExistentialProto.Type).Type#]; name=Protocol
// PROTOCOLTYPE_DOT_2-DAG: Keyword/CurrNominal:                Type[#any ExistentialProto.Type.Type#]; name=Type

  let _ = ExistentialProto.Protocol.#^PROTOCOLTYPE_DOT_3^#
// PROTOCOLTYPE_DOT_3: Begin completions, 2 items
// PROTOCOLTYPE_DOT_3-DAG: Keyword[self]/CurrNominal:          self[#(any ExistentialProto).Type.Type#]; name=self
// PROTOCOLTYPE_DOT_3-DAG: Keyword/CurrNominal:                Type[#(any ExistentialProto).Type.Type#]; name=Type
}

// rdar://problem/48141174
class TestChain {
  class Child {
    var value: Struct1
  }
  class Struct1 {
    var value: Struct2
  }
  class Struct2 {
    var prop1: Int
    var prop2: Int
  }

  var child: Child!

  func foo() {
    let _ = self.child.value.value.#^COMPLEX_CHAIN_1?check=COMPLEX_CHAIN^#
    let _ = child.value.value.#^COMPLEX_CHAIN_2?check=COMPLEX_CHAIN^#
// COMPLEX_CHAIN: Begin completions, 3 items
// COMPLEX_CHAIN-DAG: Keyword[self]/CurrNominal:          self[#Struct2#]
// COMPLEX_CHAIN-DAG: Decl[InstanceVar]/CurrNominal:      prop1[#Int#]
// COMPLEX_CHAIN-DAG: Decl[InstanceVar]/CurrNominal:      prop2[#Int#]
  }
}

// rdar://problem/48453760
struct InitializerTest {
  let value1: FooStruct = {
    $0.#^CLOSURE_IN_MEMBERDECLINIT_1?check=FOO_OBJECT_DOT^#
    return $0
  }(FooStruct())

  let value2: FooStruct = { (b: FooStruct ) -> FooStruct in
    b.#^CLOSURE_IN_MEMBERDECLINIT_2?check=FOO_OBJECT_DOT^#
    return b
  }(FooStruct())
}
// rdar://problem/40944761
extension String {
  static let v = { (obj: FooStruct) in
    obj.#^CLOSURE_IN_MEMBERDECLINIT_3?check=FOO_OBJECT_DOT^#
  }
}

struct SimpleStruct {
  func foo() -> SimpleStruct {}
}
// SIMPLE_OBJECT_DOT-DAG: Keyword[self]/CurrNominal:          self[#SimpleStruct#]; name=self
// SIMPLE_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: foo()[#SimpleStruct#]; name=foo()
func testInCollectionLiteral(value: SimpleStruct) {
  let _ = [
    value.#^IN_ARRAY_LITERAL_1?check=SIMPLE_OBJECT_DOT^#
  ]
  let _ = [
    value.#^IN_ARRAY_LITERAL_2?check=SIMPLE_OBJECT_DOT^#,
  ]
  let _: [String: String] = [
    value.#^IN_DICTIONARY_LITERAL_1?check=SIMPLE_OBJECT_DOT^#
  ]
  let _: [String: String] = [
    value.#^IN_DICTIONARY_LITERAL_2?check=SIMPLE_OBJECT_DOT^# : "test"
  ]
}

// rdar://problem/54047322
struct Resolver<T> {
  func fulfill(_ value: T) {}
}
func wrapSuccess<T>(_ onSuccess: @escaping (T) -> Void) -> (T, Bool) -> Void {
  fatalError()
}
func testWrapSuccess(promise: Int, seal: Resolver<Void>) {
  wrapSuccess(seal.fulfill)#^COMPLETE_CALL_RESULT^#
  // COMPLETE_CALL_RESULT: Pattern/CurrModule/Flair[ArgLabels]:                 ({#Void#}, {#Bool#})[#Void#]; name=()
}

protocol BrokenConformanceP {
  static func staticFunc()
  func instanceFunc()
}
extension BrokenConformanceP {
  static func staticFuncExtension() {}
}
struct BrokenConformanceS: BrokenConformanceP {
}
func testBrokenConformance(arg: BrokenConformanceS) {
  arg.#^BROKEN_CONFORMANCE?keywords=false^#
  // BROKEN_CONFORMANCE: Begin completions, 1 items
  // BROKEN_CONFORMANCE: Decl[InstanceMethod]/Super: instanceFunc()[#Void#];
}

protocol MetaProto {
  static func staticFunc() -> Int
  static var staticVar: Int { get }
  func instanceFunc() -> Int
  var intanceVar: Int { get }
}
extension MetaProto {
  static func staticFuncExtension() -> Int { 1 }
  static var staticVarExtension: Int { 1 }
  func instanceFuncExtension() -> Int { 1 }
  var intanceVarExtension: Int { 1 }
}
func testProtocolMetatype(protoProto: MetaProto.Protocol, protoType: MetaProto.Type) {
    let _ = BrokenConformanceP.#^PROTOCOLMETA_1^#
// PROTOCOLMETA_1: Begin completions, 3 items
// PROTOCOLMETA_1-DAG: Keyword[self]/CurrNominal:          self[#(any BrokenConformanceP).Type#]; name=self
// PROTOCOLMETA_1-DAG: Keyword/CurrNominal:                Protocol[#(any BrokenConformanceP).Type#]; name=Protocol
// PROTOCOLMETA_1-DAG: Keyword/CurrNominal:                Type[#any BrokenConformanceP.Type#]; name=Type
    let _ = protoProto.#^PROTOCOLMETA_2^#
// PROTOCOLMETA_2: Begin completions, 1 items
// PROTOCOLMETA_2-DAG: Keyword[self]/CurrNominal:          self[#(any MetaProto).Type#]; name=self
    let _ = protoType.#^PROTOCOLMETA_3^#
// PROTOCOLMETA_3: Begin completions, 7 items
// PROTOCOLMETA_3-DAG: Keyword[self]/CurrNominal:          self[#any MetaProto.Type#]; name=self
// PROTOCOLMETA_3-DAG: Decl[StaticMethod]/CurrNominal:     staticFunc()[#Int#]; name=staticFunc()
// PROTOCOLMETA_3-DAG: Decl[StaticVar]/CurrNominal:        staticVar[#Int#]; name=staticVar
// PROTOCOLMETA_3-DAG: Decl[InstanceMethod]/CurrNominal:   instanceFunc({#(self): MetaProto#})[#() -> Int#]; name=instanceFunc(:)
// PROTOCOLMETA_3-DAG: Decl[StaticMethod]/CurrNominal:     staticFuncExtension()[#Int#]; name=staticFuncExtension()
// PROTOCOLMETA_3-DAG: Decl[StaticVar]/CurrNominal:        staticVarExtension[#Int#]; name=staticVarExtension
// PROTOCOLMETA_3-DAG: Decl[InstanceMethod]/CurrNominal:   instanceFuncExtension({#(self): MetaProto#})[#() -> Int#]; name=instanceFuncExtension(:)
}

func testRdar90136020() {
    let a: Int64 = #^RDAR90136020^#
// RDAR90136020-NOT: name=Int64{{$}}
// RDAR90136020: Decl[Struct]/OtherModule[Swift]/IsSystem/TypeRelation[Convertible]: Int64[#Int64#]; name=Int64
// RDAR90136020-NOT: name=Int64{{$}}
}
