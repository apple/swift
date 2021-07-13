// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_UNRESOLVED | %FileCheck %s -check-prefix=METATYPE_UNRESOLVED
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_UNRESOLVED_BRACKET | %FileCheck %s -check-prefix=METATYPE_UNRESOLVED_BRACKET
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_INT | %FileCheck %s -check-prefix=METATYPE_INT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_INT_BRACKET | %FileCheck %s -check-prefix=METATYPE_INT_BRACKET
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INSTANCE_INT | %FileCheck %s -check-prefix=INSTANCE_INT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INSTANCE_INT_BRACKET | %FileCheck %s -check-prefix=INSTANCE_INT_BRACKET
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_ARCHETYPE | %FileCheck %s -check-prefix=METATYPE_ARCHETYPE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_ARCHETYPE_BRACKET | %FileCheck %s -check-prefix=METATYPE_ARCHETYPE_BRACKET
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INSTANCE_ARCHETYPE | %FileCheck %s -check-prefix=INSTANCE_ARCHETYPE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INSTANCE_ARCHETYPE_BRACKET | %FileCheck %s -check-prefix=INSTANCE_ARCHETYPE_BRACKET
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_LABEL | %FileCheck %s -check-prefix=METATYPE_LABEL
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INSTANCE_LABEL | %FileCheck %s -check-prefix=INSTANCE_LABEL
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SELF_IN_INSTANCEMETHOD | %FileCheck %s -check-prefix=SELF_IN_INSTANCEMETHOD
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SUPER_IN_INSTANCEMETHOD | %FileCheck %s -check-prefix=SUPER_IN_INSTANCEMETHOD
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SELF_IN_STATICMETHOD | %FileCheck %s -check-prefix=SELF_IN_STATICMETHOD
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SUPER_IN_STATICMETHOD | %FileCheck %s -check-prefix=SUPER_IN_STATICMETHOD

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=LABELED_SUBSCRIPT | %FileCheck %s -check-prefix=LABELED_SUBSCRIPT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TUPLE | %FileCheck %s -check-prefix=TUPLE

struct MyStruct<T> {
  static subscript(x: Int, static defValue: T) -> MyStruct<T> {
    fatalError()
  }
  subscript(x: Int, instance defValue: T) -> Int {
    fatalError()
  }
}

func test1() {
  let _ = MyStruct #^METATYPE_UNRESOLVED^#
// METATYPE_UNRESOLVED: Begin completions, 4 items
// METATYPE_UNRESOLVED-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}, {#static: _#}][#MyStruct<_>#];
// METATYPE_UNRESOLVED-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ()[#MyStruct<_>#];
// METATYPE_UNRESOLVED-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct<_>.Type#];
// METATYPE_UNRESOLVED-DAG: Keyword/CurrNominal:                .Type[#MyStruct<_>.Type#];
// METATYPE_UNRESOLVED: End completions

  let _ = MyStruct[#^METATYPE_UNRESOLVED_BRACKET^#
// METATYPE_UNRESOLVED_BRACKET: Begin completions
// METATYPE_UNRESOLVED_BRACKET-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#(x): Int#}, {#static: _#}[']'][#MyStruct<_>#];
// METATYPE_UNRESOLVED_BRACKET: End completions

  let _ = MyStruct<Int> #^METATYPE_INT^#
// METATYPE_INT: Begin completions, 4 items
// METATYPE_INT-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}, {#static: Int#}][#MyStruct<Int>#];
// METATYPE_INT-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ()[#MyStruct<Int>#];
// METATYPE_INT-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct<Int>.Type#];
// METATYPE_INT-DAG: Keyword/CurrNominal:                .Type[#MyStruct<Int>.Type#];
// METATYPE_INT: End completions

  let _ = MyStruct<Int>[#^METATYPE_INT_BRACKET^#
// METATYPE_INT_BRACKET: Begin completions
// METATYPE_INT_BRACKET-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#(x): Int#}, {#static: Int#}[']'][#MyStruct<Int>#];
// METATYPE_INT_BRACKET: End completions

  let _ = MyStruct<Int>()#^INSTANCE_INT^#
// INSTANCE_INT: Begin completions, 2 items
// INSTANCE_INT-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}, {#instance: Int#}][#Int#];
// INSTANCE_INT-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct<Int>#];
// INSTANCE_INT: End completions

  let _ = MyStruct<Int>()[#^INSTANCE_INT_BRACKET^#
// INSTANCE_INT_BRACKET: Begin completions
// INSTANCE_INT_BRACKET-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#(x): Int#}, {#instance: Int#}[']'][#Int#];
// INSTANCE_INT_BRACKET-DAG: Pattern/CurrNominal/Flair[ArgLabels]:                ['[']{#keyPath: KeyPath<MyStruct<Int>, Value>#}[']'][#Value#];
// INSTANCE_INT_BRACKET: End completions
}
func test2<U>(value: MyStruct<U>) {
  let _ = MyStruct<U>#^METATYPE_ARCHETYPE^#
// METATYPE_ARCHETYPE: Begin completions, 4 items
// METATYPE_ARCHETYPE-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}, {#static: U#}][#MyStruct<U>#];
// METATYPE_ARCHETYPE-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ()[#MyStruct<U>#];
// METATYPE_ARCHETYPE-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct<U>.Type#];
// METATYPE_ARCHETYPE-DAG: Keyword/CurrNominal:                .Type[#MyStruct<U>.Type#];
// METATYPE_ARCHETYPE: End completions

  let _ = MyStruct<U>[#^METATYPE_ARCHETYPE_BRACKET^#
// METATYPE_ARCHETYPE_BRACKET: Begin completions
// METATYPE_ARCHETYPE_BRACKET-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#(x): Int#}, {#static: U#}[']'][#MyStruct<U>#];
// METATYPE_ARCHETYPE_BRACKET: End completions

  let _ = value #^INSTANCE_ARCHETYPE^#
// INSTANCE_ARCHETYPE: Begin completions, 2 items
// INSTANCE_ARCHETYPE-DAG: Decl[Subscript]/CurrNominal:        [{#(x): Int#}, {#instance: U#}][#Int#];
// INSTANCE_ARCHETYPE-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct<U>#];
// INSTANCE_ARCHETYPE: End completions

  let _ = value[#^INSTANCE_ARCHETYPE_BRACKET^#
// INSTANCE_ARCHETYPE_BRACKET: Begin completions
// INSTANCE_ARCHETYPE_BRACKET-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#(x): Int#}, {#instance: U#}[']'][#Int#];
// INSTANCE_ARCHETYPE_BRACKET-DAG: Pattern/CurrNominal/Flair[ArgLabels]:                ['[']{#keyPath: KeyPath<MyStruct<U>, Value>#}[']'][#Value#];
// INSTANCE_ARCHETYPE_BRACKET: End completions

  let _ = MyStruct<U>[42, #^METATYPE_LABEL^#
// METATYPE_LABEL: Begin completions, 1 items
// METATYPE_LABEL-DAG: Pattern/Local/Flair[ArgLabels]: {#static: U#}[#U#];
// METATYPE_LABEL: End completions

  let _ = value[42, #^INSTANCE_LABEL^#
// INSTANCE_LABEL: Begin completions, 1 items
// INSTANCE_LABEL-DAG: Pattern/Local/Flair[ArgLabels]: {#instance: U#}[#U#];
// INSTANCE_LABEL: End completions
}

class Base {
  static subscript(static x: Int) -> Int { return 1 }
  subscript(instance x: Int) -> Int { return 1 }
}
class Derived: Base {
  static subscript(derivedStatic x: Int) -> Int { return 1 }
  subscript(derivedInstance x: Int) -> Int { return 1 }

  func testInstance() {
    let _ = self[#^SELF_IN_INSTANCEMETHOD^#]
// SELF_IN_INSTANCEMETHOD: Begin completions, 3 items
// SELF_IN_INSTANCEMETHOD-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]: ['[']{#derivedInstance: Int#}[']'][#Int#];
// SELF_IN_INSTANCEMETHOD-DAG: Decl[Subscript]/Super/Flair[ArgLabels]:       ['[']{#instance: Int#}[']'][#Int#];
// SELF_IN_INSTANCEMETHOD-DAG: Pattern/CurrNominal/Flair[ArgLabels]:         ['[']{#keyPath: KeyPath<Derived, Value>#}[']'][#Value#];
// SELF_IN_INSTANCEMETHOD: End completions

    let _ = super[#^SUPER_IN_INSTANCEMETHOD^#]
// SUPER_IN_INSTANCEMETHOD: Begin completions, 2 items
// SUPER_IN_INSTANCEMETHOD-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]: ['[']{#instance: Int#}[']'][#Int#];
// SUPER_IN_INSTANCEMETHOD-DAG: Pattern/CurrNominal/Flair[ArgLabels]:         ['[']{#keyPath: KeyPath<Base, Value>#}[']'][#Value#];
// SUPER_IN_INSTANCEMETHOD: End completions
  }

  static func testStatic() {
    let _ = self[#^SELF_IN_STATICMETHOD^#]
// SELF_IN_STATICMETHOD: Begin completions, 3 items
// SELF_IN_STATICMETHOD-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]: ['[']{#derivedStatic: Int#}[']'][#Int#];
// SELF_IN_STATICMETHOD-DAG: Decl[Subscript]/Super/Flair[ArgLabels]:       ['[']{#static: Int#}[']'][#Int#];
// SELF_IN_STATICMETHOD-DAG: Pattern/CurrNominal/Flair[ArgLabels]:         ['[']{#keyPath: KeyPath<Derived.Type, Value>#}[']'][#Value#];
// SELF_IN_STATICMETHOD: End completions

    let _ = super[#^SUPER_IN_STATICMETHOD^#]
// SUPER_IN_STATICMETHOD: Begin completions, 2 items
// SUPER_IN_STATICMETHOD-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]: ['[']{#static: Int#}[']'][#Int#];
// SUPER_IN_STATICMETHOD-DAG: Pattern/CurrNominal/Flair[ArgLabels]:         ['[']{#keyPath: KeyPath<Base.Type, Value>#}[']'][#Value#];
// SUPER_IN_STATICMETHOD: End completions
  }
}

struct MyStruct1<X: Comparable> {
  subscript(idx1 _: Int, idx2 _: X) -> Int! { return 1 }
}
func testSubscriptCallSig<T>(val: MyStruct1<T>) {
  val[#^LABELED_SUBSCRIPT^#
// LABELED_SUBSCRIPT: Begin completions, 2 items
// LABELED_SUBSCRIPT-DAG: Decl[Subscript]/CurrNominal/Flair[ArgLabels]:        ['[']{#idx1: Int#}, {#idx2: Comparable#}[']'][#Int!#];
// LABELED_SUBSCRIPT-DAG: Pattern/CurrNominal/Flair[ArgLabels]:                ['[']{#keyPath: KeyPath<MyStruct1<T>, Value>#}[']'][#Value#];
// LABELED_SUBSCRIPT: End completions
}

func testSubcscriptTuple(val: (x: Int, String)) {
  val[#^TUPLE^#]
// TUPLE: Begin completions, 1 items
// TUPLE-DAG: Pattern/CurrNominal/Flair[ArgLabels]:                 ['[']{#keyPath: KeyPath<(x: Int, String), Value>#}[']'][#Value#];
// TUPLE: End completions
}
