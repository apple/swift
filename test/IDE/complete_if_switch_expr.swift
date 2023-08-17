// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -debug-forbid-typecheck-prefix FORBIDDEN -filecheck %raw-FileCheck -completion-output-dir %t

enum E {
  case e
  case f(Int)
}

struct SomeError: Error {}

func ifExprDotReturn() -> E {
  if .random() {
    .#^DOT1?check=DOT^#
  } else {
    .e
  }
}

func switchExprDotReturn() -> E {
  switch Bool.random() {
  case true:
    .e
  case false:
    .#^DOT2?check=DOT^#
  }
}

func ifExprDotClosureReturn() -> E {
  let x: E = {
    if .random() {
      .e
    } else {
      .#^DOT3?check=DOT^#
    }
  }()
  return x
}

func switchExprDotClosureReturn() -> E {
  let x: E = {
    switch Bool.random() {
    case true:
      .#^DOT4?check=DOT^#
    case false:
      .e
    }
  }()
  return x
}

func ifExprBranchInferenceReturn1() -> E {
  let fn = {
    if .random() {
      E.e
    } else {
      .#^DOT5?check=DOT^#
    }
  }
  return fn()
}

func switchExprBranchInferenceReturn1() -> E {
  let fn = {
    switch Bool.random() {
    case true:
      E.e
    case false:
      .#^DOT6?check=DOT^#
    }
  }
  return fn()
}

func ifExprBranchInferenceReturn2() -> E {
  let fn = {
    if .random() {
      .#^DOT7?check=DOT^#
    } else {
      E.e
    }
  }
  return fn()
}

func switchExprBranchInferenceReturn2() -> E {
  let fn = {
    switch Bool.random() {
    case true:
      .#^DOT8?check=DOT^#
    case false:
      E.e
    }
  }
  return fn()
}

func ifExprBinding() -> E {
  let x: E = 
    if .random() {
      .e
    } else {
      .#^DOT9?check=DOT^#
    }
  return x
}

func switchExprBinding() -> E {
  let x: E =
    switch Bool.random() {
      case true:
        .e
      case false:
        .#^DOT10?check=DOT^#
    }
  return x
}

// Triggering interface type computation of this will cause an assert to fire,
// ensuring we don't attempt to type-check any references to it.
let NOTYPECHECK = FORBIDDEN

func testSkipTypechecking1() -> E {
  // Make sure we don't try to type-check the first branch, as it doesn't
  // contribute to the type.
  if .random() {
    _ = NOTYPECHECK
    throw SomeError()
  } else {
    .#^DOT11?check=DOT^#
  }
}

func testSkipTypechecking2() -> E {
  // Make sure we don't try to type-check the second branch, as it doesn't
  // contribute to the type.
  if .random() {
    .#^DOT12?check=DOT^#
  } else {
    _ = NOTYPECHECK
    throw SomeError()
  }
}

func testSkipTypechecking3() -> E {
  // Make sure we don't try to type-check the first branch, as it doesn't
  // contribute to the type.
  switch Bool.random() {
  case true:
    _ = NOTYPECHECK
    throw SomeError()
  case false:
    .#^DOT13?check=DOT^#
  }
}

func testSkipTypechecking4() -> E {
  // Make sure we don't try to type-check the second branch, as it doesn't
  // contribute to the type.
  switch Bool.random() {
  case true:
    .#^DOT14?check=DOT^#
  case false:
    _ = NOTYPECHECK
    throw SomeError()
  }
}

func takesE(_ e: E) {}

func testSkipTypechecking5() throws -> E {
  // Make sure we only type-check the first branch.
  if .random() {
    // We can still skip type-checking this tho.
    x = NOTYPECHECK

    takesE(.#^DOT15?check=DOT^#)
    throw SomeError()
  } else {
    NOTYPECHECK
  }
}

func testSkipTypechecking6() throws -> E {
  // Make sure we only type-check the first branch.
  switch Bool.random() {
  case true:
    // We can still skip type-checking this tho.
    x = NOTYPECHECK

    takesE(.#^DOT16?check=DOT^#)
    throw SomeError()
  case false:
    NOTYPECHECK
  }
}

enum F {
  case x
}

func takesIntOrDouble(_ i: Int) -> E { E.e }
func takesIntOrDouble(_ i: Double) -> F { F.x }

func testSkipTypechecking7() throws -> E {
  let fn = {
    switch Bool.random() {
    case true:
      .#^DOT17?check=DOT^#
    case false:
      // Make sure we don't end up with an ambiguity here.
      takesIntOrDouble(0)
    }
  }
  return fn()
}

func testSkipTypeChecking8() throws -> E {
  let e: E = if Bool.random() {
    takesE(.#^DOT18?check=DOT^#)
    throw NOTYPECHECK
  } else {
    NOTYPECHECK
  }
  return e
}

func testSkipTypeChecking8() throws -> E {
  // Only need to type-check the inner function in this case.
  let e: E = if Bool.random() {
    func localFunc() {
      takesE(.#^DOT19?check=DOT^#)
    }
    throw NOTYPECHECK
  } else {
    NOTYPECHECK
  }
  return e
}

func takesArgAndClosure<T>(_ x: Int, fn: () -> T) -> T { fatalError() }

func testSkipTypeChecking9() -> E {
  // We need to type-check everything for this.
  if Bool.random() {
    .e
  } else {
    takesArgAndClosure(0) {
      .#^DOT20?check=DOT^#
    }
  }
}

func testSkipTypeChecking10() -> E {
  // We only need to type-check the inner-most function for this.
  if Bool.random() {
    NOTYPECHECK
  } else {
    takesArgAndClosure(NOTYPECHECK) {
      func foo() {
        takesE(.#^DOT21?check=DOT^#)
      }
      return NOTYPECHECK
    }
  }
}

func testSkipTypeChecking11() -> E {
  // We only need to type-check the inner-most function for this.
  if Bool.random() {
    NOTYPECHECK
  } else {
    if NOTYPECHECK {
      func foo() {
        takesE(.#^DOT22?check=DOT^#)
      }
      throw NOTYPECHECK
    } else {
      NOTYPECHECK
    }
  }
}

func testSkipTypeChecking12() -> E {
  // Only need to type-check the condition here.
  if .#^DOT23?check=BOOL^# {
    NOTYPECHECK
  } else {
    NOTYPECHECK
  }
}

// BOOL:     Begin completions
// BOOL-DAG: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Convertible]: init()[#Bool#]; name=init()
// BOOL-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/IsSystem/TypeRelation[Convertible]: random()[#Bool#]; name=random()

func testSkipTypeChecking13(_ e: E) -> E {
  // TODO: Currently this requires type-checking the bodies, but we ought to
  // be able to skip them. This is also an issue in closures.
  switch e {
  case .#^DOT24?check=DOT^#:
      .e
  default:
      .e
  }
}

func testSkipTypeChecking14() -> E {
  if Bool.random() {
    .#^DOT25?check=DOT^#
  } else if .random() {
    let x = NOTYPECHECK
    throw x
  } else if .random() {
    let x = NOTYPECHECK
    throw x
  } else {
    let x = NOTYPECHECK
    throw x
  }
}

func testSkipTypeChecking14() -> E {
  switch Bool.random() {
  case true:
    .#^DOT26?check=DOT^#
  case _ where Bool.random():
    let x = NOTYPECHECK
    throw x
  case _ where Bool.random():
    let x = NOTYPECHECK
    throw x
  default:
    let x = NOTYPECHECK
    throw x
  }
}

func testSkipTypechecking15(_ x: inout Int) -> E {
  switch Bool.random() {
  case true:
    .#^DOT27?check=DOT^#
  case false:
    x = 0
  }
}

// DOT:     Begin completions, 2 items
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: e[#E#]; name=e
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: f({#Int#})[#E#]; name=f()
