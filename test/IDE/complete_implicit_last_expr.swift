// RUN: %batch-code-completion -enable-experimental-feature ImplicitLastExprResults -debug-forbid-typecheck-prefix FORBIDDEN

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

func test1() -> E {
  if .random() {
    ();
    .#^DOT1?check=DOT^#
  } else {
    .e
  }
}

func test2() -> E {
  switch Bool.random() {
  case true:
    .e
  case false:
    ();
    .#^DOT2?check=DOT^#
  }
}

func test3() throws -> E {
  switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    ();
    .#^DOT3?check=DOT^#
  }
}

struct S {
  var e: E
}

func test4() throws -> E {
  let x = switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    let s = S(e: .e)
    s.#^SDOT1?check=SDOT^#
  }
  return x
}

func test5() -> E {
  ();
  .#^DOT4?check=DOT^#
}

func test6() -> E {
  let fn: () -> E = {
    ();
    .#^DOT5?check=DOT^#
  }
  return fn()
}

func test7() throws -> E {
  print("hello")
  switch Bool.random() {
  case true:
    NO.TYPECHECK
    throw NO.TYPECHECK
  case false:
    ();
    .#^DOT7?check=DOT^#
  }
}

// SDOT:     Begin completions, 2 items
// SDOT-DAG: Keyword[self]/CurrNominal:          self[#S#]; name=self
// SDOT-DAG: Decl[InstanceVar]/CurrNominal:      e[#E#]; name=e

// DOT:     Begin completions, 2 items
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: e[#E#]; name=e
// DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: f({#Int#})[#E#]; name=f()

struct TestImplicitLastExprMember {
  func void() -> Void {}
  func str() -> String { return "" }
  func int() -> Int { return 0 }

  func test1() -> Int {
    print("hello")
    #^TestImplicitLastExprMember1?check=IMPLICIT_LAST_EXPR_MEMBER^#
  }

  func test2() -> Int {
    print("hello")
    self.#^TestImplicitLastExprMember2?check=IMPLICIT_LAST_EXPR_MEMBER^#
  }

  func test3() -> Int {
    {
      print("hello")
      #^TestImplicitLastExprMember3?check=IMPLICIT_LAST_EXPR_MEMBER^#
    }()
  }

  func test4() -> Int {
    ();
    {
      print("hello")
      self.#^TestImplicitLastExprMember4?check=IMPLICIT_LAST_EXPR_MEMBER^#
    }()
  }

  // IMPLICIT_LAST_EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal:   void()[#Void#]; name=void()
  // IMPLICIT_LAST_EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal:   str()[#String#]; name=str()
  // IMPLICIT_LAST_EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Convertible]: int()[#Int#]; name=int()

  func test5() {
    let fn = {
      print("hello")
      self.#^TestImplicitLastExprMember5?check=IMPLICIT_LAST_EXPR_MEMBER_VOID^#
    }
  }

  func test6() {
    let fn: () -> Void = {
      print("hello")
      #^TestImplicitLastExprMember6?check=IMPLICIT_LAST_EXPR_MEMBER_VOID^#
    }
  }

  // IMPLICIT_LAST_EXPR_MEMBER_VOID-DAG: Decl[InstanceMethod]/CurrNominal: void()[#Void#]; name=void()
  // IMPLICIT_LAST_EXPR_MEMBER_VOID-DAG: Decl[InstanceMethod]/CurrNominal: str()[#String#]; name=str()
  // IMPLICIT_LAST_EXPR_MEMBER_VOID-DAG: Decl[InstanceMethod]/CurrNominal: int()[#Int#]; name=int()
}
