// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_A | %FileCheck %s -check-prefix=TEST_A
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_B | %FileCheck %s -check-prefix=TEST_B
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_C | %FileCheck %s -check-prefix=TEST_C
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_D | %FileCheck %s -check-prefix=TEST_D
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_D_DOT | %FileCheck %s -check-prefix=TEST_D_DOT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TEST_D_PAREN | %FileCheck %s -check-prefix=TEST_D_PAREN
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METATYPE_CONVERSION | %FileCheck %s -check-prefix=METATYPE_CONVERSION

class A {
  init(int i: Int) {}
  init(double d: Double) {}
  convenience init(float f: Float) {
    self.init(Double(f))
  }
}

// TEST_A-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#int: Int#})[#A#]{{; name=.+$}}
// TEST_A-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#double: Double#})[#A#]{{; name=.+$}}
// TEST_A-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#float: Float#})[#A#]{{; name=.+$}}
// TEST_A-DAG: Keyword[self]/CurrNominal:          .self[#A.Type#]; name=self
// TEST_A-DAG: Keyword/CurrNominal:                .Type[#A.Type#]; name=Type

class B : A {
  var x = 0
  // A's subobject initializers are implicitly overridden.
  // A's complete object initializers are inherited.
}

// TEST_B-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#int: Int#})[#B#]{{; name=.+$}}
// TEST_B-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#double: Double#})[#B#]{{; name=.+$}}
// TEST_B-DAG: Decl[Constructor]/Super/Flair[ArgLabels]:            ({#float: Float#})[#A#]{{; name=.+$}}
// TEST_B-DAG: Keyword[self]/CurrNominal:          .self[#B.Type#]; name=self
// TEST_B-DAG: Keyword/CurrNominal: .Type[#B.Type#]; name=Type

class C : B {
  init(int i: Int) {
    super.init(int: i)
  }
  convenience init(c other: C) {
    self.init(int: 0)
  }
  // No initializers are inherited.
}

// TEST_C-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#int: Int#})[#C#]{{; name=.+$}}
// TEST_C-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#c: C#})[#C#]{{; name=.+$}}
// TEST_C-DAG: Keyword[self]/CurrNominal:          .self[#C.Type#]; name=self
// TEST_C-DAG: Keyword/CurrNominal:                .Type[#C.Type#]; name=Type

class D : C {
  var y = 0
  // C's subobject initializers are implicitly overridden.
  // C's complete object initializers are inherited.
  // Initializers from A are not included in D's interface.

  convenience init(d other: D) {
    self.init(int: 0)
  }
}

// TEST_D-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#d: D#})[#D#]{{; name=.+$}}
// TEST_D-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ({#int: Int#})[#D#]{{; name=.+$}}
// TEST_D-DAG: Decl[Constructor]/Super/Flair[ArgLabels]:            ({#c: C#})[#C#]{{; name=.+$}}
// TEST_D-DAG: Keyword[self]/CurrNominal:          .self[#D.Type#]; name=self
// TEST_D-DAG: Keyword/CurrNominal:                .Type[#D.Type#]; name=Type

// TEST_D_DOT: Decl[Constructor]/CurrNominal:       init({#d: D#})[#D#]; name=init(d:)
// TEST_D_DOT-DAG: Decl[Constructor]/CurrNominal:  init({#int: Int#})[#D#]; name=init(int:)
// TEST_D_DOT-DAG: Decl[Constructor]/Super:        init({#c: C#})[#C#]; name=init(c:)

// TEST_D_PAREN: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:       ['(']{#d: D#}[')'][#D#]; name=d:
// TEST_D_PAREN-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:  ['(']{#int: Int#}[')'][#D#]; name=int:
// TEST_D_PAREN-DAG: Decl[Constructor]/Super/Flair[ArgLabels]:  ['(']{#c: C#}[')'][#D#]; name=c:

func testA() {
  A#^TEST_A^#
}

func testB() {
  B#^TEST_B^#
}

func testC() {
  C#^TEST_C^#
}

func testD() {
  D#^TEST_D^#
  D.#^TEST_D_DOT^#
  D(#^TEST_D_PAREN^#
}

class R74233797Base {
    init() {}
    convenience init(_ test: Bool) { self.init() }
}
class R74233797Derived : R74233797Base {
    convenience init(sub: Bool) { self.init(sub) }
}
func testR74233797() {
    R74233797Derived(#^METATYPE_CONVERSION^#)
// METATYPE_CONVERSION-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#sub: Bool#}[')'][#R74233797Derived#];
// METATYPE_CONVERSION-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#R74233797Derived#];
// METATYPE_CONVERSION-DAG: Decl[Constructor]/Super/Flair[ArgLabels]: ['(']{#(test): Bool#}[')'][#R74233797Derived#];
}
