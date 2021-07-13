// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

struct A {
  func doAThings() -> A { return self }
}

struct B {
  func doBThings() {}
}

func overloadedReturn() -> A { return A() }
func overloadedReturn() -> B { return B() }

overloadedReturn().#^SIMPLE^#
overloadedReturn(1).#^SIMPLE_EXTRAARG?check=SIMPLE^#

struct HasMembers {
  func overloadedReturn() -> A { return A() }
  func overloadedReturn() -> B { return B() }
}

HasMembers().overloadedReturn().#^SIMPLE_MEMBERS?check=SIMPLE^#

func givenErrorExpr(_ a: String) -> A {}
func givenErrorExpr(_ b: Int) -> B {}

func arrayWrapper<T>(a: T) -> [T]
arrayWrapper(overloadedReturn()).#^SKIP_DUPLICATES^#

// SKIP_DUPLICATES: Begin completions
// SKIP_DUPLICATES-NOT: count[#Int#]
// SKIP_DUPLICATES-NOT: formIndex({#(i): &Int#}, {#offsetBy: Int#})[#Void#]
// SKIP_DUPLICATES: Decl[InstanceVar]/CurrNominal/IsSystem: count[#Int#]{{; name=.+$}}
// SKIP_DUPLICATES: Decl[InstanceMethod]/Super/IsSystem: formIndex({#(i): &Int#}, {#offsetBy: Int#})[#Void#]{{; name=.+$}}
// SKIP_DUPLICATES-NOT: count[#Int#]
// SKIP_DUPLICATES-NOT: formIndex({#(i): &Int#}, {#offsetBy: Int#})[#Void#]
// SKIP_DUPLICATES: End completions

let x: (inout Int, Int) -> () = arrayWrapper(overloadedReturn()).#^SKIP_COMPOUND_DUPLICATES^#

// SKIP_COMPOUND_DUPLICATES: Begin completions
// SKIP_COMPOUND_DUPLICATES: Decl[InstanceMethod]/Super/IsSystem/TypeRelation[Identical]: formIndex(_:offsetBy:)[#(inout Int, Int) -> ()#]{{; name=.+$}}
// SKIP_COMPOUND_DUPLICATES-NOT: formIndex(_:offsetBy:)[#(inout Int, Int) -> ()#]
// SKIP_COMPOUND_DUPLICATES: End completions

func testCallAsFunctionDeduplication() {
  struct Test<T> {
    func callAsFunction(x: Int) {}
  }

  func overloaded() -> Test<A> { fatalError() }
  func overloaded() -> Test<B> { fatalError() }

  overloaded()#^SKIP_CALLASFUNCTION_DUPLICATES^#
}

// FIXME: Update this to check the callAsFunction pattern only appears once when PostfixExpr completion is migrated to the solver-based implementation (which handles ambiguity).
// SKIP_CALLASFUNCTION_DUPLICATES-NOT: Begin completions

givenErrorExpr(undefined).#^ERROR_IN_BASE?check=SIMPLE^#

// SIMPLE: Begin completions, 4 items
// SIMPLE-DAG: Keyword[self]/CurrNominal:          self[#A#]{{; name=.+$}}
// SIMPLE-DAG: Decl[InstanceMethod]/CurrNominal:   doAThings()[#A#]{{; name=.+$}}
// SIMPLE-DAG: Keyword[self]/CurrNominal:          self[#B#]{{; name=.+$}}
// SIMPLE-DAG: Decl[InstanceMethod]/CurrNominal:   doBThings()[#Void#]{{; name=.+$}}
// SIMPLE: End completions

let x: A = overloadedReturn().#^RELATED^#
let x: A = overloadedReturn(1).#^RELATED_EXTRAARG?check=RELATED^#

// RELATED: Begin completions, 4 items
// RELATED-DAG: Keyword[self]/CurrNominal:          self[#A#]{{; name=.+$}}
// RELATED-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: doAThings()[#A#]{{; name=.+$}}
// RELATED-DAG: Keyword[self]/CurrNominal:          self[#B#]{{; name=.+$}}
// RELATED-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: doBThings()[#Void#]{{; name=.+$}}
// RELATED: End completions

func takesClosureGivingA(_ callback: () -> A) -> B {}
func takesB(_ item: B) {}

takesB((takesClosureGivingA { return overloadedReturn().#^RELATED_INERROREXPR?check=RELATED^# }).)

func overloadedArg(_ arg: A) -> A {}
func overloadedArg(_ arg: B) -> B {}

overloadedArg(.#^UNRESOLVED_AMBIGUOUS^#)

// UNRESOLVED_AMBIGUOUS: Begin completions, 4 items
// UNRESOLVED_AMBIGUOUS-DAG: Decl[InstanceMethod]/CurrNominal: doAThings({#(self): A#})[#() -> A#]{{; name=.+$}}
// UNRESOLVED_AMBIGUOUS-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init()[#A#]{{; name=.+$}}
// UNRESOLVED_AMBIGUOUS-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: doBThings({#(self): B#})[#() -> Void#]{{; name=.+$}}
// UNRESOLVED_AMBIGUOUS-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init()[#B#]{{; name=.+$}}
// UNRESOLVED_AMBIGUOUS: End completions

// Make sure we still offer A and B members as the user may intend to add a member on the end of the overloadedArg call later that has type B.
takesB(overloadedArg(.#^UNRESOLVED_STILLAMBIGUOUS?check=UNRESOLVED_AMBIGUOUS^#))

func overloadedArg2(_ arg: A) -> Void {}
func overloadedArg2(_ arg: A) -> Never {}
func overloadedArg2(_ arg: B) -> B {}

takesB(overloadedArg2(.#^UNRESOLVED_UNAMBIGUOUS^#))

// UNRESOLVED_UNAMBIGUOUS: Begin completions, 2 items
// UNRESOLVED_UNAMBIGUOUS-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: doBThings({#(self): B#})[#() -> Void#]{{; name=.+$}}
// UNRESOLVED_UNAMBIGUOUS-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init()[#B#]{{; name=.+$}}
// UNRESOLVED_UNAMBIGUOUS: End completions


switch undefined {
  case takesClosureGivingA { return overloadedReturn().#^NOCALLBACK_FALLBACK?check=RELATED^# }:
    break
}

func takesClosureA(_ arg: (A) -> ()) {}
func takesClosureB(_ arg: (B) -> ()) {}

takesClosureA { arg in
  takesClosureB { arg in
    arg.#^MULTICLOSURE_FALLBACK^#
  }
  print() + 10
} + 10

// MULTICLOSURE_FALLBACK: Begin completions, 2 items
// MULTICLOSURE_FALLBACK-DAG: Keyword[self]/CurrNominal:        self[#B#]{{; name=.+$}}
// MULTICLOSURE_FALLBACK-DAG: Decl[InstanceMethod]/CurrNominal: doBThings()[#Void#]{{; name=.+$}}
// MULTICLOSURE_FALLBACK: End completions

func takesAnonClosure(_ x: (A) -> A) { return A() }
func takesAnonClosure(_ x: (B, A) -> B { return B() }
func takesAnonClosure(_ x: () -> (A, B) { return (A(), B()) }

struct TestRelations {
  static let a = A()
  static let b = B()
  static let ab = (A(), B())
}

// test we consider both overloads as $0 or $1 may have just not been written yet
takesAnonClosure { $1.#^UNAMBIGUOUSCLOSURE_ARG^# }
// UNAMBIGUOUSCLOSURE_ARG: Begin completions, 2 items
// UNAMBIGUOUSCLOSURE_ARG-DAG: Keyword[self]/CurrNominal: self[#A#]{{; name=.+$}}
// UNAMBIGUOUSCLOSURE_ARG-DAG: Decl[InstanceMethod]/CurrNominal: doAThings()[#A#]{{; name=.+$}}
// UNAMBIGUOUSCLOSURE_ARG: End completions

takesAnonClosure { $0.#^AMBIGUOUSCLOSURE_ARG^# }
// AMBIGUOUSCLOSURE_ARG: Begin completions, 4 items
// AMBIGUOUSCLOSURE_ARG-DAG: Keyword[self]/CurrNominal: self[#A#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: doAThings()[#A#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG-DAG: Keyword[self]/CurrNominal: self[#B#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG-DAG: Decl[InstanceMethod]/CurrNominal: doBThings()[#Void#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG: End completions

takesAnonClosure { TestRelations.#^AMBIGUOUSCLOSURE_ARG_RETURN^# }
// AMBIGUOUSCLOSURE_ARG_RETURN: Begin completions, 6 items
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Keyword[self]/CurrNominal: self[#TestRelations.Type#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Keyword/CurrNominal: Type[#TestRelations.Type#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: a[#A#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: b[#B#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: ab[#(A, B)#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN-DAG: Decl[Constructor]/CurrNominal: init()[#TestRelations#]{{; name=.+$}}
// AMBIGUOUSCLOSURE_ARG_RETURN: End completions

func takesDictAB(_ x: [A: B]) {}
func takesOptDictAB(_ x: [A: B]?) {}
func overloadedGivesAorB(_ x: A) -> A {}
func overloadedGivesAorB(_ x: B) -> B {}
var assignDict: [A : B] = [:]

let _: [A : B] = [TestRelations.#^PARSED_AS_ARRAY?check=PARSED_AS_ARRAY_KEY^#]
let _: [A : B]? = [TestRelations.#^PARSED_AS_ARRAY_OPTIONAL?check=PARSED_AS_ARRAY_KEY^#]
let _: [[A : B]] = [[TestRelations.#^PARSED_AS_ARRAY_NESTED?check=PARSED_AS_ARRAY_KEY^#]]
assignDict = [TestRelations.#^PARSED_AS_ARRAY_ASSIGN?check=PARSED_AS_ARRAY_KEY^#]
let _: [A: B] = [overloadedGivesAorB(TestRelations.#^PARSED_AS_ARRAY_INDIRECT?check=PARSED_AS_ARRAY_KEY^#)]
let _: [[A: B]] = [[overloadedGivesAorB(TestRelations.#^PARSED_AS_ARRAY_INDIRECT_NESTED?check=PARSED_AS_ARRAY_KEY^#)]]
takesDictAB([overloadedGivesAorB(TestRelations.#^PARSED_AS_ARRAY_INDIRECT_CALL?check=PARSED_AS_ARRAY_KEY^#)]);
takesOptDictAB([overloadedGivesAorB(TestRelations.#^PARSED_AS_ARRAY_INDIRECT_CALL_OPT?check=PARSED_AS_ARRAY_KEY^#)]);
func testReturnLiteralMismatch() -> [A: B] { return [overloadedGivesAorB(TestRelations.#^PARSED_AS_ARRAY_INDIRECT_RETURN?check=PARSED_AS_ARRAY_KEY^#)] }
func arrayLiteralDictionaryMismatch<T>(a: inout T) where T: ExpressibleByDictionaryLiteral, T.Key == A, T.Value == B {
  a = [TestRelations.#^PARSED_AS_ARRAY_GENERIC?check=PARSED_AS_ARRAY_KEY^#]
}

// PARSED_AS_ARRAY_KEY: Begin completions, 6 items
// PARSED_AS_ARRAY_KEY-DAG: Keyword[self]/CurrNominal: self[#TestRelations.Type#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY-DAG: Keyword/CurrNominal: Type[#TestRelations.Type#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: a[#A#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY-DAG: Decl[StaticVar]/CurrNominal: b[#B#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY-DAG: Decl[StaticVar]/CurrNominal: ab[#(A, B)#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY-DAG: Decl[Constructor]/CurrNominal: init()[#TestRelations#]{{; name=.+$}}
// PARSED_AS_ARRAY_KEY: End completions

let _: [(A, B) : B] = [TestRelations.#^PARSED_AS_ARRAY_TUPLE^#]
let _: [(A, B)] = [TestRelations.#^PARSED_AS_ARRAY_ARRAY?check=PARSED_AS_ARRAY_TUPLE^#]
// PARSED_AS_ARRAY_TUPLE: Begin completions, 6 items
// PARSED_AS_ARRAY_TUPLE-DAG: Keyword[self]/CurrNominal: self[#TestRelations.Type#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE-DAG: Keyword/CurrNominal: Type[#TestRelations.Type#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE-DAG: Decl[StaticVar]/CurrNominal: a[#A#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE-DAG: Decl[StaticVar]/CurrNominal: b[#B#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: ab[#(A, B)#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE-DAG: Decl[Constructor]/CurrNominal: init()[#TestRelations#]{{; name=.+$}}
// PARSED_AS_ARRAY_TUPLE: End completions


func testMissingArgs() {
  enum Foo { case foo }
  enum Bar { case bar }

  struct Test {
    static let foo = Foo.foo
    static let bar = Bar.bar
  }

  func test(foo: Foo) {}
  func test(bar: Bar) {}

  func test2(first: Bar, second: Int) {}
  func test2(first: Foo) {}

  func test3(skipMe: Int, after: Foo) {}
  func test3(after: Bar) {}

  func test4(skipMe: Int, both: Foo, skipMeToo: Int) {}
  func test4(both: Bar, skipMeTo: Int) {}


  test(foo: Test.#^OVERLOADEDFUNC_FOO^#)
  // OVERLOADEDFUNC_FOO: Begin completions, 5 items
  // OVERLOADEDFUNC_FOO-DAG: Keyword[self]/CurrNominal: self[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_FOO-DAG: Keyword/CurrNominal: Type[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_FOO-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: foo[#Foo#]{{; name=.+$}}
  // OVERLOADEDFUNC_FOO-DAG: Decl[StaticVar]/CurrNominal: bar[#Bar#]{{; name=.+$}}
  // OVERLOADEDFUNC_FOO-DAG: Decl[Constructor]/CurrNominal: init()[#Test#]{{; name=.+$}}
  // OVERLOADEDFUNC_FOO: End completions

  test(bar: Test.#^OVERLOADEDFUNC_BAR^#)
  // OVERLOADEDFUNC_BAR: Begin completions, 5 items
  // OVERLOADEDFUNC_BAR-DAG: Keyword[self]/CurrNominal: self[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_BAR-DAG: Keyword/CurrNominal: Type[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_BAR-DAG: Decl[StaticVar]/CurrNominal: foo[#Foo#]{{; name=.+$}}
  // OVERLOADEDFUNC_BAR-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: bar[#Bar#]{{; name=.+$}}
  // OVERLOADEDFUNC_BAR-DAG: Decl[Constructor]/CurrNominal: init()[#Test#]{{; name=.+$}}
  // OVERLOADEDFUNC_BAR: End completions

  test(Test.#^OVERLOADEDFUNC_MISSINGLABEL?check=OVERLOADEDFUNC_BOTH^#, extraArg: 2)
  test2(first: Test.#^OVERLOADEDFUNC_MISSINGARG_AFTER?check=OVERLOADEDFUNC_BOTH^#)

  // Also check ambiguous member functions
  struct TestStruct {
    func test2(first: Bar, second: Int) {}
    func test2(first: Foo) {}
  }

  TestStruct().test2(first: Test.#^OVERLOADEDMEMBER_MISSINGARG_AFTER?check=OVERLOADEDFUNC_BOTH^#)

  // TODO: Should we insert the missing label in the completion text for OVERLOADEDFUNC_MISSINGLABEL?
  // OVERLOADEDFUNC_BOTH: Begin completions, 5 items
  // OVERLOADEDFUNC_BOTH-DAG: Keyword[self]/CurrNominal: self[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_BOTH-DAG: Keyword/CurrNominal: Type[#Test.Type#]{{; name=.+$}}
  // OVERLOADEDFUNC_BOTH-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: foo[#Foo#]{{; name=.+$}}
  // OVERLOADEDFUNC_BOTH-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: bar[#Bar#]{{; name=.+$}}
  // OVERLOADEDFUNC_BOTH-DAG: Decl[Constructor]/CurrNominal: init()[#Test#]{{; name=.+$}}
  // OVERLOADEDFUNC_BOTH: End completions

  test3(after: Test.#^OVERLOADEDFUNC_MISSINGARG_BEFORE?check=OVERLOADEDFUNC_BAR^#);
  test4(both: Test.#^OVERLOADEDFUNC_MISSINGARG_BEFOREANDAFTER?check=OVERLOADEDFUNC_BAR^#)

  enum Bop { case bop }
  enum Bix { case bix }
  enum Blu { case blu }
  enum Baz { case baz }
  enum Boy { case boy }

  func trailing(x: Int, _ y: () -> Foo, z: () -> ())  {}
  func trailing(x: Int, _ y: () -> Bar, z: (() -> ())?)  {}
  func trailing(x: Int, _ y: () -> Bop, z: Any)  {}
  func trailing<T>(x: Int, _ y: () -> Bix, z: T) {}
  func trailing<T>(x: Int, _ y: () -> Boy, z: T?) {}
  func trailing<T>(x: Int, _ y: () -> Blu, z: [T]?) {}
  func trailing(x: Int, _ y: () -> Blu, z: inout Any)  {}
  func trailing(x: Int, _ y: () -> Baz, z: Int)  {}

  trailing(x: 2, { .#^MISSINGARG_INLINE^# })
  trailing(x: 2) { .#^MISSINGARG_TRAILING^# }

  // MISSINGARG_INLINE: Begin completions, 14 items
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: foo[#Foo#]; name=foo
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Foo#})[#(into: inout Hasher) -> Void#]; name=hash(self: Foo)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bar[#Bar#]; name=bar
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bar#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bar)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bop[#Bop#]; name=bop
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bop#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bop)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bix[#Bix#]; name=bix
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bix#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bix)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: boy[#Boy#]; name=boy
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Boy#})[#(into: inout Hasher) -> Void#]; name=hash(self: Boy)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: blu[#Blu#]; name=blu
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Blu#})[#(into: inout Hasher) -> Void#]; name=hash(self: Blu)
  // MISSINGARG_INLINE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: baz[#Baz#]; name=baz
  // MISSINGARG_INLINE-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Baz#})[#(into: inout Hasher) -> Void#]; name=hash(self: Baz)
  // MISSINGARG_INLINE: End completions

  // MISSINGARG_TRAILING: Begin completions, 10 items
  // MISSINGARG_TRAILING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: foo[#Foo#]; name=foo
  // MISSINGARG_TRAILING-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Foo#})[#(into: inout Hasher) -> Void#]; name=hash(self: Foo)
  // MISSINGARG_TRAILING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bar[#Bar#]; name=bar
  // MISSINGARG_TRAILING-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bar#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bar)
  // MISSINGARG_TRAILING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bop[#Bop#]; name=bop
  // MISSINGARG_TRAILING-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bop#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bop)
  // MISSINGARG_TRAILING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bix[#Bix#]; name=bix
  // MISSINGARG_TRAILING-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Bix#})[#(into: inout Hasher) -> Void#]; name=hash(self: Bix)
  // MISSINGARG_TRAILING-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: boy[#Boy#]; name=boy
  // MISSINGARG_TRAILING-DAG: Decl[InstanceMethod]/CurrNominal:   hash({#(self): Boy#})[#(into: inout Hasher) -> Void#]; name=hash(self: Boy)
  // MISSINGARG_TRAILING: End completions
}

protocol C {
  associatedtype Element
  func getCElem() -> Element
}

protocol D {
  associatedtype Item
  func getDElem() -> Item
}

struct CDStruct: C, D {
  func getDElem() -> A { return A() }
  func getCElem() -> B { return B() }
}

func genericReturn<T:C, U>(_ x: T) -> U where U == T.Element {
  return x.getCElem()
}
func genericReturn<T:D, U>(_ x: T) -> U where U == T.Item {
  return x.getDElem()
}

genericReturn(CDStruct()).#^GENERIC^#

// GENERIC: Begin completions, 4 items
// GENERIC-DAG: Keyword[self]/CurrNominal:          self[#B#]{{; name=.+$}}
// GENERIC-DAG: Decl[InstanceMethod]/CurrNominal:   doBThings()[#Void#]{{; name=.+$}}
// GENERIC-DAG: Keyword[self]/CurrNominal:          self[#A#]{{; name=.+$}}
// GENERIC-DAG: Decl[InstanceMethod]/CurrNominal:   doAThings()[#A#]{{; name=.+$}}
// GENERIC: End completions

genericReturn().#^GENERIC_MISSINGARG?check=NORESULTS^#

// NORESULTS-NOT: Begin completions

struct Point {
    let x: Int
    let y: Int
    var magSquared: Int { return x*y }

    init(_ x: Int, _ y: Int) {
        self.x = x
        self.y = y
    }
}

// POINT_MEMBER: Begin completions, 4 items
// POINT_MEMBER-DAG: Keyword[self]/CurrNominal:          self[#Point#]; name=self
// POINT_MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      x[#Int#]; name=x
// POINT_MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      y[#Int#]; name=y
// POINT_MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      magSquared[#Int#]; name=magSquared
// POINT_MEMBER: End completions

let _ = [Point(1, 4), Point(20, 2), Point(9, -4)]
  .filter { $0.magSquared > 4 }
  .min {
    $0.#^CLOSURE_MISSINGARG?check=POINT_MEMBER^#
  }

protocol SomeProto {}
func testing<T: Collection, U: SomeProto>(_ arg1: T, arg2: (T.Element) -> U) {}
_ = testing([Point(4, 89)]) { arg in
  arg.#^CLOSURE_NORETURN?check=POINT_MEMBER^#
}

struct Thing {
    init(_ block: (Point) -> Void) {}
}
@resultBuilder
struct ThingBuilder {
    static func buildBlock(_ x: Thing...) -> [Thing] { x }
}
func CreateThings(@ThingBuilder makeThings: () -> [Thing]) {}

// In single statement closure
CreateThings {
    Thing { point in
      point.#^CLOSURE_FUNCBUILDER?check=POINT_MEMBER^#
    }
    Thing { _ in }
}

// In multi-statement closure
CreateThings {
    Thing { point in
      print("hello")
      point.#^MULTICLOSURE_FUNCBUILDER?check=POINT_MEMBER^#
    }
    Thing { _ in }
}

// In multi-statement closure with unpropagated errors
CreateThings {
    Thing { point in
      print("hello")
      point. // ErrorExpr
      point.#^MULTICLOSURE_FUNCBUILDER_ERROR?check=POINT_MEMBER^#
    }
    Thing { point in 
      print("hello")
      point. // ErrorExpr
    }
}

// FIXME: No results in multi-statement closure with erroreous sibling result builder element
CreateThings {
    Thing { point in
      print("hello")
      point.#^MULTICLOSURE_FUNCBUILDER_FIXME?check=NORESULTS^#
    }
    Thing. // ErrorExpr
}


func takesClosureOfPoint(_: (Point)->()) {}
func overloadedWithDefaulted(_: ()->()) {}
func overloadedWithDefaulted(_: ()->(), _ defaulted: Int = 10) {}

takesClosureOfPoint { p in
  overloadedWithDefaulted {
    if p.#^REGULAR_MULTICLOSURE_APPLIED?check=POINT_MEMBER^# {}
  }
}

enum Enum123 {
    case enumElem
}
struct Struct123: Equatable {
    var structMem = Enum123.enumElem
}
func testBestSolutionFilter() {
  let a = Struct123();
  let b = [Struct123]().first(where: { $0 == a && 1 + 90 * 5 / 8 == 45 * -10 })?.structMem != .#^BEST_SOLUTION_FILTER?xfail=rdar73282163^#
  let c = min(10.3, 10 / 10.4) < 6 / 7 ? true : Optional(a)?.structMem != .#^BEST_SOLUTION_FILTER2?check=BEST_SOLUTION_FILTER;xfail=rdar73282163^#
}

// BEST_SOLUTION_FILTER: Begin completions
// BEST_SOLUTION_FILTER-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: enumElem[#Enum123#]{{; name=.+$}}
// BEST_SOLUTION_FILTER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): Enum123#})[#(into: inout Hasher) -> Void#]{{; name=.+$}}
// BEST_SOLUTION_FILTER-DAG: Keyword[nil]/None/Erase[1]/TypeRelation[Identical]: nil[#Enum123?#]{{; name=.+$}}
// BEST_SOLUTION_FILTER-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Identical]: none[#Optional<Enum123>#]{{; name=.+$}}
// BEST_SOLUTION_FILTER-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Identical]: some({#Enum123#})[#Optional<Enum123>#]{{; name=.+$}}
// BEST_SOLUTION_FILTER: End completions

func testBestSolutionGeneric() {
  struct Test1 {}
  func genAndInt(_ x: Int) -> Int { return 1 }
  func genAndInt<T>(_ x: T) -> Test1 { return Test1() }

  genAndInt(2).#^BEST_SOLUTION_FILTER_GEN?xfail=rdar73282163^#
}

// BEST_SOLUTION_FILTER_GEN: Begin completions
// BEST_SOLUTION_FILTER_GEN-DAG: Keyword[self]/CurrNominal:     self[#Int#]; name=self
// BEST_SOLUTION_FILTER_GEN-DAG: Keyword[self]/CurrNominal:     self[#Test1#]; name=self
// BEST_SOLUTION_FILTER_GEN: End completions

func testAmbiguousArgs() {
  struct A {
    func someFunc(a: Int, b: Int) -> A { return self }
  }

  struct B {
    func someFunc(a: Int, c: String = "", d: String = "") {}
  }

  func overloaded() -> A { return A() }
  func overloaded() -> B { return B() }

  let localInt = 10
  let localString = "Hello"

  overloaded().someFunc(a: 2, #^ARG_NO_LABEL^#)

  // ARG_NO_LABEL: Begin completions, 3 items
  // ARG_NO_LABEL-DAG: Pattern/Local/Flair[ArgLabels]:     {#b: Int#}[#Int#]; name=b: Int
  // ARG_NO_LABEL-DAG: Pattern/Local/Flair[ArgLabels]:     {#c: String#}[#String#]; name=c: String
  // ARG_NO_LABEL-DAG: Pattern/Local/Flair[ArgLabels]:     {#d: String#}[#String#]; name=d: String
  // ARG_NO_LABEL: End completions

  overloaded().someFunc(a: 2, b: #^ARG_LABEL^#)

  // ARG_LABEL: Begin completions
  // ARG_LABEL-DAG: Decl[LocalVar]/Local:                          localString[#String#]; name=localString
  // ARG_LABEL-DAG: Decl[LocalVar]/Local/TypeRelation[Identical]:  localInt[#Int#]; name=localInt
  // ARG_LABEL: End completions

  overloaded().someFunc(a: 2, c: "Foo", #^ARG_NO_LABEL2^#)

  // ARG_NO_LABEL2: Begin completions, 1 item
  // ARG_NO_LABEL2: Pattern/Local/Flair[ArgLabels]:     {#d: String#}[#String#]; name=d: String
  // ARG_NO_LABEL2: End completions

  overloaded().someFunc(a: 2, wrongLabel: "Foo", #^ARG_NO_LABEL_PREV_WRONG^#)

  // ARG_NO_LABEL_PREV_WRONG: Begin completions, 2 items
  // ARG_NO_LABEL_PREV_WRONG-DAG: Pattern/Local/Flair[ArgLabels]:     {#c: String#}[#String#]; name=c: String
  // ARG_NO_LABEL_PREV_WRONG-DAG: Pattern/Local/Flair[ArgLabels]:     {#d: String#}[#String#]; name=d: String
  // ARG_NO_LABEL_PREV_WRONG: End completions

  overloaded().someFunc(a: 2, d: "Foo", #^ARG_NO_LABEL_OUT_OF_ORDER^#)
  // ARG_NO_LABEL_OUT_OF_ORDER: Begin completions
  // ARG_NO_LABEL_OUT_OF_ORDER: Pattern/Local/Flair[ArgLabels]:     {#c: String#}[#String#]; name=c: String
  // ARG_NO_LABEL_OUT_OF_ORDER-NOT: name=d: String
  // ARG_NO_LABEL_OUT_OF_ORDER: End completions

  func noArgs() {}
  noArgs(12, #^ARG_EXTRANEOUS^#)
  // ARG_EXTRANEOUS: Begin completions
  // ARG_EXTRANEOUS-DAG: localInt
  // ARG_EXTRANEOUS-DAG: localString
  // ARG_EXTRANEOUS: End completions
}

