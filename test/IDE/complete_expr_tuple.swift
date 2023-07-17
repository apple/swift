// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_NO_DOT_1 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_NO_DOT_1 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_NO_DOT_2 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_NO_DOT_2 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_NO_DOT_3 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_NO_DOT_3 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_DOT_1 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_DOT_1 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_DOT_2 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_DOT_2 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_DOT_3 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_DOT_3 < %t.tuple.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE_NESTED_1 > %t.tuple.txt
// RUN: %FileCheck %s -check-prefix=TUPLE_NESTED_1 < %t.tuple.txt

//===---
//===--- Test code completion for expressions that have tuple type.
//===---

func testTupleNoDot1() {
  var t = (1, 2.0)
  t#^TUPLE_NO_DOT_1^#
}
// TUPLE_NO_DOT_1: Begin completions, 10 items
// TUPLE_NO_DOT_1-DAG: Pattern/CurrNominal: .0[#Int#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Pattern/CurrNominal: .1[#Double#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: == {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: <= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: >= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: < {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: != {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: > {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: BuiltinOperator/None:                                   = {#(Int, Double)#}{{; name=.+$}}
// TUPLE_NO_DOT_1-DAG: Keyword[self]/CurrNominal: .self[#(Int, Double)#]; name=self

func testTupleNoDot2() {
  var t = (foo: 1, bar: 2.0)
  t#^TUPLE_NO_DOT_2^#
}
// TUPLE_NO_DOT_2: Begin completions, 10 items
// TUPLE_NO_DOT_2-DAG: Pattern/CurrNominal: .foo[#Int#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Pattern/CurrNominal: .bar[#Double#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: == {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: <= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: >= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: < {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: != {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: > {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: BuiltinOperator/None:                                   = {#(foo: Int, bar: Double)#}{{; name=.+$}}
// TUPLE_NO_DOT_2-DAG: Keyword[self]/CurrNominal: .self[#(foo: Int, bar: Double)#]; name=self

func testTupleNoDot3() {
  var t = (foo: 1, 2.0)
  t#^TUPLE_NO_DOT_3^#
}
// TUPLE_NO_DOT_3: Begin completions, 10 items
// TUPLE_NO_DOT_3-DAG: Pattern/CurrNominal: .foo[#Int#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Pattern/CurrNominal: .1[#Double#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: == {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: <= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: >= {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: < {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: != {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: > {#(Int, Double)#}[#Bool#]{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: BuiltinOperator/None:                                   = {#(foo: Int, Double)#}{{; name=.+$}}
// TUPLE_NO_DOT_3-DAG: Keyword[self]/CurrNominal: .self[#(foo: Int, Double)#]; name=self

func testTupleDot1() {
  var t = (1, 2.0)
  t.#^TUPLE_DOT_1^#
}
// TUPLE_DOT_1: Begin completions, 3 items
// TUPLE_DOT_1-NEXT: Keyword[self]/CurrNominal: self[#(Int, Double)#]; name=self
// TUPLE_DOT_1-NEXT: Pattern/CurrNominal: 0[#Int#]{{; name=.+$}}
// TUPLE_DOT_1-NEXT: Pattern/CurrNominal: 1[#Double#]{{; name=.+$}}

func testTupleDot2() {
  var t = (foo: 1, bar: 2.0)
  t.#^TUPLE_DOT_2^#
}
// TUPLE_DOT_2: Begin completions, 3 items
// TUPLE_DOT_2-NEXT: Keyword[self]/CurrNominal: self[#(foo: Int, bar: Double)#]; name=self
// TUPLE_DOT_2-NEXT: Pattern/CurrNominal: foo[#Int#]{{; name=.+$}}
// TUPLE_DOT_2-NEXT: Pattern/CurrNominal: bar[#Double#]{{; name=.+$}}

func testTupleDot3() {
  var t = (foo: 1, 2.0)
  t.#^TUPLE_DOT_3^#
}
// TUPLE_DOT_3: Begin completions, 3 items
// TUPLE_DOT_3-NEXT: Keyword[self]/CurrNominal: self[#(foo: Int, Double)#]; name=self
// TUPLE_DOT_3-NEXT: Pattern/CurrNominal: foo[#Int#]{{; name=.+$}}
// TUPLE_DOT_3-NEXT: Pattern/CurrNominal: 1[#Double#]{{; name=.+$}}

struct FooStruct {
  var fooInstanceVar: Int = 0
  var barInstanceVar: Double = 0.0
}

func testTupleNested1() {
  var t = (foo: FooStruct(), i: Int)
  t.foo.#^TUPLE_NESTED_1^#
}
// TUPLE_NESTED_1: Begin completions, 3 items
// TUPLE_NESTED_1-NEXT: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// TUPLE_NESTED_1-NEXT: Decl[InstanceVar]/CurrNominal: fooInstanceVar[#Int#]{{; name=.+$}}
// TUPLE_NESTED_1-NEXT: Decl[InstanceVar]/CurrNominal: barInstanceVar[#Double#]{{; name=.+$}}
