// RUN: %batch-code-completion -enable-experimental-feature ThenStatements -debug-forbid-typecheck-prefix FORBIDDEN

// Experimental feature requires asserts
// REQUIRES: asserts

enum E {
  case e
  case f(Int)
}

struct NO {
  // Triggering interface type computation of this will cause an assert to fire,
  // ensuring we don't attempt to type-check any references to it.
  static var TYPECHECK = FORBIDDEN
}

func testThenStmt1() -> E {
  if .random() {
    ()
    then .#^DOT1?check=DOT^#
  } else {
    .e
  }
}

func testThenStmt2() -> E {
  switch Bool.random() {
  case true:
    .e
  case false:
    ()
    then .#^DOT2?check=DOT^#
  }
}

func testThenStmt3() throws -> E {
  switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    ()
    then .#^DOT3?check=DOT^#
  }
}

func testThenStmt4() throws -> E {
  switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    then #^NODOT1?check=NODOT^#
  }
}

// NODOT:     Begin completions
// NODOT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .e[#E#]; name=e
// NODOT-DAG: Decl[EnumElement]/CurrNominal/TypeRelation[Convertible]: .f({#Int#})[#E#]; name=f()

struct S {
  var e: E
}

func testThenStmt5() throws -> E {
  let x = switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    let s = S(e: .e)
    then s.#^SDOT1?check=SDOT^#
  }
  return x
}

// SDOT:     Begin completions, 2 items
// SDOT-DAG: Keyword[self]/CurrNominal:          self[#S#]; name=self
// SDOT-DAG: Decl[InstanceVar]/CurrNominal:      e[#E#]; name=e

// DOT:     Begin completions, 2 items
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: e[#E#]; name=e
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: f({#Int#})[#E#]; name=f()
