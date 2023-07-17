// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

//===--- Helper types that are used in this test

struct FooStruct {
}

var fooObject : FooStruct

func fooFunc() -> FooStruct {
  return fooObject
}

enum FooEnum {
}

class FooClass {
}

protocol FooProtocol {
  var fooInstanceVar : Int
  typealias FooTypeAlias1
  func fooInstanceFunc0() -> Double
  func fooInstanceFunc1(a: Int) -> Double
  subscript(i: Int) -> Double
}

protocol BarProtocol {
  var barInstanceVar : Int
  typealias BarTypeAlias1
  func barInstanceFunc0() -> Double
  func barInstanceFunc1(a: Int) -> Double
}

typealias FooTypealias = Int

// GLOBAL-DAG: fooObject
// GLOBAL-DAG: fooFunc
// GLOBAL-DAG: FooTypealias
// GLOBAL-DAG: FooProtocol
// GLOBAL-DAG: FooClass

// GLOBAL_NEGATIVE-NOT: fooObject
// GLOBAL_NEGATIVE-NOT: fooFunc

//===---
//===--- Test that we don't try to suggest anything where pattern-atom is expected.
//===---

// NO_COMPLETIONS-NOT: Begin completions

// Use do { } to reset the parser after broken syntax.
do { var #^PATTERN_ATOM_1?check=NO_COMPLETIONS^# }
do { var (#^PATTERN_ATOM_2?check=NO_COMPLETIONS^# }
do {var (a, #^PATTERN_ATOM_3?check=NO_COMPLETIONS^# }
do {var (a #^PATTERN_ATOM_4?check=NO_COMPLETIONS^# }
do {var ((#^PATTERN_ATOM_5?check=NO_COMPLETIONS^# }
do {var ((a, b), #^PATTERN_ATOM_6?check=NO_COMPLETIONS^# }
do {guard var #^PATTERN_ATOM_7?check=NO_COMPLETIONS^# }
do {guard var #^PATTERN_ATOM_8?check=NO_COMPLETIONS^# else { fatalError() } }
do {guard let #^PATTERN_ATOM_9?check=NO_COMPLETIONS^# else { fatalError() } }
do {guard let a = Optional(1), let #^PATTERN_ATOM_10?check=NO_COMPLETIONS^# else { fatalError() } }
do {if let #^PATTERN_ATOM_11?check=NO_COMPLETIONS^# {} }
do {if let a = Optional(1), let #^PATTERN_ATOM_12?check=NO_COMPLETIONS^# {} }
do {if case #^PATTERN_IF_CASE_1?check=GLOBAL^# {} }
do {if case let #^PATTERN_IF_CASE_2?check=NO_COMPLETIONS^# {} }

func inFunc() {
  do { var #^PATTERN_INFUNC_ATOM_1?check=NO_COMPLETIONS^# }
  do { var (#^PATTERN_INFUNC_ATOM_2?check=NO_COMPLETIONS^# }
  do {var (a, #^PATTERN_INFUNC_ATOM_3?check=NO_COMPLETIONS^# }
  do {var (a #^PATTERN_INFUNC_ATOM_4?check=NO_COMPLETIONS^# }
  do {var ((#^PATTERN_INFUNC_ATOM_5?check=NO_COMPLETIONS^# }
  do {var ((a, b), #^PATTERN_INFUNC_ATOM_6?check=NO_COMPLETIONS^# }
  do {guard var #^PATTERN_INFUNC_ATOM_7?check=NO_COMPLETIONS^# }
  do {guard var #^PATTERN_INFUNC_ATOM_8?check=NO_COMPLETIONS^# else { fatalError() } }
  do {guard let #^PATTERN_INFUNC_ATOM_9?check=NO_COMPLETIONS^# else { fatalError() } }
  do {guard let a = Optional(1), let #^PATTERN_INFUNC_ATOM_10?check=NO_COMPLETIONS^# else { fatalError() } }
  do {if let #^PATTERN_INFUNC_ATOM_11?check=NO_COMPLETIONS^# {} }
  do {if let a = Optional(1), let #^PATTERN_INFUNC_ATOM_12?check=NO_COMPLETIONS^# {} }
  do {if case #^PATTERN_INFUNC_IF_CASE_1?check=GLOBAL^# {} }
  do {if case let #^PATTERN_INFUNC_IF_CASE_2?check=NO_COMPLETIONS^# {} }
}

//===---
//===--- Test that we complete the type in 'is' pattern.
//===---

func patternIs1(x: FooClass) {
  switch x {
  case is #^PATTERN_IS_1?check=GLOBAL_NEGATIVE^#
  }
}

func patternIs2() {
  switch unknown_var {
  case is #^PATTERN_IS_2?check=GLOBAL_NEGATIVE^#
  }
}

func patternIs3() {
  switch {
  case is #^PATTERN_IS_3?check=GLOBAL_NEGATIVE^#
  }
}

//===--- Test that we include types from generic parameter lists.

func patternIsGeneric1<
    GenericFoo : FooProtocol,
    GenericBar : FooProtocol & BarProtocol,
    GenericBaz>(x: FooClass) {
  switch x {
  case is #^PATTERN_IS_GENERIC_1?check=GLOBAL_NEGATIVE;check=PATTERN_IS_GENERIC_1^#
  }
}

// Generic parameters of the function.
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

struct PatternIsGeneric2<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  func patternIsGeneric2<
      GenericFoo : FooProtocol,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(x: FooClass) {
    switch x {
    case is #^PATTERN_IS_GENERIC_2?check=GLOBAL_NEGATIVE;check=PATTERN_IS_GENERIC_2^#
    }
  }
}

// Generic parameters of the struct.
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}
// Generic parameters of the function.
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// PATTERN_IS_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}


// rdar://21174713
// AFTER_PATTERN_IS: Begin completions
func test<T>(x: T) {
  switch T.self {
  case is Int.Type:
    #^AFTER_PATTERN_IS^#
  }
}

func test_multiple_patterns1(x: Int) {
    switch (x, x) {
    case (0, let a), #^MULTI_PATTERN_1^#
    }
}

// MULTI_PATTERN_1-NOT: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_1-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}

func test_multiple_patterns2(x: Int) {
    switch (x, x) {
    case (0, let a), (let a, 0):
        #^MULTI_PATTERN_2^#
    }
}

// MULTI_PATTERN_2-DAG: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_2-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}

func test_multiple_patterns3(x: Int) {
    switch (x, x) {
    case (0, let a), (let b, 0):
        #^MULTI_PATTERN_3^#
    }
}

// MULTI_PATTERN_3-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}

func test_multiple_patterns4(x: Int) {
    switch (x, x) {
    case (0, let a) where #^MULTI_PATTERN_4^#
        
    }
}

// MULTI_PATTERN_4-DAG: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// MULTI_PATTERN_4-DAG: Decl[LocalVar]/Local: x[#Int#]{{; name=.+$}}

enum IntHolder {
  case hold(Int)
}
func ident(int: Int) -> Int { return int }
func ident(double: Double) -> Int { return Double }

func test_cc_in_pattern(subject: IntHolder, i1: Int) {
  switch subject {
  case .hold(#^CC_IN_PATTERN_1^#):
    ()
  }
}

// CC_IN_PATTERN_1-DAG: Decl[LocalVar]/Local/TypeRelation[Convertible]: i1[#Int#]; name=i1

func testCompleteAfterPatternInClosure() {
  func takeClosure(_ x: () -> Void) {}

  enum MyEnum {
    case failure(Int)
  }

  func test(value: MyEnum) {
    takeClosure {
      switch value {
      case let .failure(error)#^AFTER_PATTERN_IN_CLOSURE^#:
        break
      }
    }
  }

  // AFTER_PATTERN_IN_CLOSURE-NOT: Begin completions
}

func testIfLetInClosure(foo: Int?) {
  func takeClosure(_ x: () -> Void) {}

  takeClosure {
    if let items#^IF_LET_IN_CLOSURE^# = foo {
    }
  }
  // IF_LET_IN_CLOSURE-NOT: Begin completions
}
