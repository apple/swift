// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

var i1 = 1
var i2 = 2
var oi1 : Int?
var oi2 : Int?
var s1 = ""
var s2 = ""
var os1 : String?
var os2 : String?
func voidGen() {}
func ointGen() -> Int? { return 1 }
func intGen() -> Int {return 1}
func ostringGen() -> String? {return ""}
func stringGen() -> String {return ""}
func foo(_ a : Int) {}
func foo(_ a : Int, b1 :Int?) {}
func foo(_ a : Int, b2 :Int?, b3: Int?) {}
func foo1(_ a : Int, b : Int) {}
func bar(_ a : String, b : String?) {}
func bar1(_ a : String, b1 : String) {}
func bar1(_ a : String, b2 : String) {}
func foo3(_ a: Int?) {}

class InternalGen {
  func InternalIntGen() -> Int { return 0 }
  func InternalIntOpGen() -> Int? {return 0 }
  func InternalStringGen() -> String { return "" }
  func InternalStringOpGen() -> String? {return ""}
  func InternalIntTaker(_ i1 : Int, i2 : Int) {}
  func InternalStringTaker(_ s1: String, s2 : String) {}
}

class Gen {
  var IG = InternalGen()
  func IntGen() -> Int { return 0 }
  func IntOpGen() -> Int? {return 0 }
  func StringGen() -> String { return "" }
  func StringOpGen() -> String? {return ""}
  func IntTaker(_ i1 : Int, i2 : Int) {}
  func StringTaker(_ s1: String, s2 : String) {}
}

func GenGenerator(_ i : Int) -> Gen { return Gen() }

enum SimpleEnum {
  case foo, bar, baz
}

class C1 {
  func f1() {
    foo(3, b: #^ARG1?check=EXPECT_OINT^#)
  }
  func f2() {
    foo(3, #^ARG2?check=ARG-NAME1^#)
  }
  func f3() {
    foo1(2, #^ARG3?check=ARG-NAME2^#
  }
  func f4() {
    foo1(2, b : #^ARG4?check=EXPECT_INT^#
  }

  func f5() {
    foo(#^FARG1?check=EXPECT_INT^#, b1 : 2)
  }

  func f6() {
    bar(#^FARG2?check=EXPECT_STRING^#
  }

  func f7() {
    foo3(#^FARG7?check=EXPECT_OINT^#)
  }
}

// ARG-NAME1: Begin completions, 2 items
// ARG-NAME1-DAG: Pattern/Local/Flair[ArgLabels]: {#b1: Int?#}[#Int?#];
// ARG-NAME1-DAG: Pattern/Local/Flair[ArgLabels]: {#b2: Int?#}[#Int?#];

// ARG-NAME2: Begin completions, 1 items
// ARG-NAME2-DAG: Pattern/Local/Flair[ArgLabels]: {#b: Int#}[#Int#];

// ARG-NAME3: Begin completions, 1 items
// ARG-NAME3-DAG: Pattern/Local/Flair[ArgLabels]: {#b: String?#}[#String?#];

// ARG-NAME4: Begin completions, 2 items
// ARG-NAME4-DAG: Pattern/Local/Flair[ArgLabels]: {#b1: String#}[#String#];
// ARG-NAME4-DAG: Pattern/Local/Flair[ArgLabels]: {#b2: String#}[#String#];
// ARG-NAME4: End completions

// EXPECT_OINT: Begin completions
// EXPECT_OINT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// EXPECT_OINT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f2()[#Void#]; name=f2()
// EXPECT_OINT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: i2[#Int#]; name=i2
// EXPECT_OINT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: i1[#Int#]; name=i1
// EXPECT_OINT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: oi2[#Int?#]; name=oi2
// EXPECT_OINT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: oi1[#Int?#]; name=oi1
// EXPECT_OINT-DAG: Decl[GlobalVar]/CurrModule:         os1[#String?#]; name=os1
// EXPECT_OINT-DAG: Keyword[try]/None: try; name=try
// EXPECT_OINT-DAG: Keyword[try]/None: try!; name=try!
// EXPECT_OINT-DAG: Keyword[try]/None: try?; name=try?
// EXPECT_OINT-DAG: Keyword/None: await; name=await
// EXPECT_OINT-NOT: Keyword[super]
// EXPECT_OINT: End completions

// EXPECT_INT: Begin completions
// EXPECT_INT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// EXPECT_INT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f2()[#Void#]; name=f2()
// EXPECT_INT-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Invalid]: voidGen()[#Void#]; name=voidGen()
// EXPECT_INT-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Identical]: intGen()[#Int#]; name=intGen()
// EXPECT_INT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i1[#Int#]; name=i1
// EXPECT_INT-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i2[#Int#]; name=i2
// EXPECT_INT-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem/TypeRelation[Identical]: Int[#Int#]
// EXPECT_INT-DAG: Decl[FreeFunction]/CurrModule:      ointGen()[#Int?#]; name=ointGen()
// EXPECT_INT-DAG: Decl[GlobalVar]/CurrModule:         oi1[#Int?#]; name=oi1
// EXPECT_INT-DAG: Decl[GlobalVar]/CurrModule:         os2[#String?#]; name=os2
// EXPECT_INT-DAG: Decl[GlobalVar]/CurrModule:         oi2[#Int?#]; name=oi2
// EXPECT_INT-DAG: Keyword[try]/None: try; name=try
// EXPECT_INT-DAG: Keyword[try]/None: try!; name=try!
// EXPECT_INT-DAG: Keyword[try]/None: try?; name=try?
// EXPECT_INT-DAG: Keyword/None: await; name=await
// EXPECT_INT-NOT: Keyword[super]
// EXPECT_INT: End completions

class C2 {
  func f1() {
    bar("", b: #^ARG5?check=EXPECT_OSTRING^#)
  }
  func f2() {
    bar("", #^ARG6?check=ARG-NAME3^#)
  }
  func f3() {
    bar1("", #^ARG7?check=ARG-NAME4^#
  }
  func f4() {
    bar1("", b : #^ARG8?check=EXPECT_STRING^#
  }
}

// EXPECT_OSTRING: Begin completions
// EXPECT_OSTRING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// EXPECT_OSTRING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f2()[#Void#]; name=f2()
// EXPECT_OSTRING-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Convertible]: stringGen()[#String#]; name=stringGen()
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: s2[#String#]; name=s2
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: s1[#String#]; name=s1
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: os1[#String?#]; name=os1
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: os2[#String?#]; name=os2
// EXPECT_OSTRING-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Identical]: ostringGen()[#String?#]; name=ostringGen()
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule:         i1[#Int#]; name=i1
// EXPECT_OSTRING-DAG: Decl[GlobalVar]/CurrModule:         i2[#Int#]; name=i2
// EXPECT_OSTRING-DAG: Keyword[try]/None: try; name=try
// EXPECT_OSTRING-DAG: Keyword[try]/None: try!; name=try!
// EXPECT_OSTRING-DAG: Keyword[try]/None: try?; name=try?
// EXPECT_OSTRING-DAG: Keyword/None: await; name=await
// EXPECT_OSTRING-NOT: Keyword[super]
// EXPECT_OSTRING: End completions

// EXPECT_STRING: Begin completions
// EXPECT_STRING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// EXPECT_STRING-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f2()[#Void#]; name=f2()
// EXPECT_STRING-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Identical]: stringGen()[#String#]; name=stringGen()
// EXPECT_STRING-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem/TypeRelation[Identical]: String[#String#]
// EXPECT_STRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s1[#String#]; name=s1
// EXPECT_STRING-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s2[#String#]; name=s2
// EXPECT_STRING-DAG: Decl[GlobalVar]/CurrModule:         os1[#String?#]; name=os1
// EXPECT_STRING-DAG: Decl[GlobalVar]/CurrModule:         os2[#String?#]; name=os2
// EXPECT_STRING-DAG: Keyword[try]/None: try; name=try
// EXPECT_STRING-DAG: Keyword[try]/None: try!; name=try!
// EXPECT_STRING-DAG: Keyword[try]/None: try?; name=try?
// EXPECT_STRING-DAG: Keyword/None: await; name=await
// EXPECT_STRING-NOT: Keyword[super]
// EXPECT_STRING: End completions

func foo2(_ a : C1, b1 : C2) {}
func foo2(_ a : C2, b2 : C1) {}

class C3 {
  var C1I = C1()
  var C2I = C2()
  func f1() {
    foo2(C1I, #^OVERLOAD1^#)
  }
  func f2() {
    foo2(C2I, #^OVERLOAD2^#)
  }
  func f3() {
    foo2(C1I, b1: #^OVERLOAD3^#)
  }
  func f4() {
    foo2(C2I, b2: #^OVERLOAD4^#)
  }

  func f5() {
    foo2(#^OVERLOAD5^#
  }

  func overloaded(_ a1: C1, b1: C2) {}
  func overloaded(a2: C2, b2: C1) {}

  func f6(obj: C3) {
    overloaded(#^OVERLOAD6^#
    func sync() {}
    obj.overloaded(#^OVERLOAD7?check=OVERLOAD6^#
  }
}

// OVERLOAD1: Begin completions, 1 items
// OVERLOAD1-NEXT: Pattern/Local/Flair[ArgLabels]: {#b1: C2#}[#C2#]; name=b1: C2
// OVERLOAD1-NEXT: End completions

// OVERLOAD2: Begin completions, 1 items
// OVERLOAD2-NEXT: Pattern/Local/Flair[ArgLabels]: {#b2: C1#}[#C1#]; name=b2: C1
// OVERLOAD2-NEXT: End completions

// OVERLOAD3: Begin completions
// OVERLOAD3-DAG: Decl[InstanceVar]/CurrNominal:      C1I[#C1#]; name=C1I
// OVERLOAD3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// OVERLOAD3-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C2I[#C2#]; name=C2I
// OVERLOAD3-DAG: Decl[Class]/CurrModule/TypeRelation[Identical]: C2[#C2#]
// OVERLOAD3: End completions

// FIXME: This should be a negative test case
// NEGATIVE_OVERLOAD_3-NOT: Decl[Class]{{.*}} C1

// OVERLOAD4: Begin completions
// OVERLOAD4-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C1I[#C1#]; name=C1I
// OVERLOAD4-DAG: Decl[InstanceVar]/CurrNominal:      C2I[#C2#]; name=C2I
// OVERLOAD4-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: f1()[#Void#]; name=f1()
// OVERLOAD4-DAG: Decl[Class]/CurrModule/TypeRelation[Identical]: C1[#C1#]
// OVERLOAD4: End completions

// FIXME: This should be a negative test case
// NEGATIVE_OVERLOAD4-NOT: Decl[Class]{{.*}} C2

// OVERLOAD5: Begin completions
// OVERLOAD5-DAG: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]:      ['(']{#(a): C1#}, {#b1: C2#}[')'][#Void#]; name=a: C1, b1: C2
// OVERLOAD5-DAG: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]:      ['(']{#(a): C2#}, {#b2: C1#}[')'][#Void#]; name=a: C2, b2: C1
// OVERLOAD5-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C1I[#C1#]; name=C1I
// OVERLOAD5-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C2I[#C2#]; name=C2I
// OVERLOAD5: End completions

// OVERLOAD6: Begin completions
// OVERLOAD6-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#(a1): C1#}, {#b1: C2#}[')'][#Void#]; name=a1: C1, b1: C2
// OVERLOAD6-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#a2: C2#}, {#b2: C1#}[')'][#Void#]; name=a2: C2, b2: C1
// OVERLOAD6-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C1I[#C1#]; name=C1I
// OVERLOAD6-DAG: Decl[InstanceVar]/CurrNominal:      C2I[#C2#]; name=C2I
// OVERLOAD6: End completions

extension C3 {
  func hasError(a1: C1, b1: TypeInvalid) -> Int {}

  func f7(obj: C3) {
    let _ = obj.hasError(#^HASERROR1^#
    let _ = obj.hasError(a1: #^HASERROR2?xfail=FIXME^#
    let _ = obj.hasError(a1: IC1, #^HASERROR3?xfail=FIXME^#
    let _ = obj.hasError(a1: IC1, b1: #^HASERROR4^#
  }
}

// HASERROR1: Begin completions
// HASERROR1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#a1: C1#}, {#b1: <<error type>>#}[')'][#Int#];
// HASERROR1: End completions

// HASERROR2: Begin completions
// HASERROR2-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Identical]: C1I[#C1#];
// HASERROR2-DAG: Decl[InstanceVar]/CurrNominal:      C2I[#C2#];
// HASERROR2: End completions

// HASERROR3: Begin completions
// HASERROR3-DAG: Pattern/Local/Flair[ArgLabels]: {#b1: <<error type>>#}[#<<error type>>#];
// HASERROR3: End completions

// HASERROR4: Begin completions
// HASERROR4-DAG: Decl[InstanceVar]/CurrNominal:      C1I[#C1#];
// HASERROR4-DAG: Decl[InstanceVar]/CurrNominal:      C2I[#C2#];
// HASERROR4: End completions

class C4 {
  func f1(_ G : Gen) {
    foo(1, b1: G.#^MEMBER1^#
  }

  func f2(_ G : Gen) {
    foo1(2, b : G.#^MEMBER2^#
  }

  func f3(_ G : Gen) {
    bar("", b1 : G.#^MEMBER3^#
  }

  func f4(_ G : Gen) {
    bar1("", b1 : G.#^MEMBER4^#
  }

  func f5(_ G1 : Gen, G2 : Gen) {
    G1.IntTaker(1, i1 : G2.#^MEMBER5?check=MEMBER2^#
  }

  func f6(_ G1 : Gen, G2 : Gen) {
    G1.StringTaker("", s2: G2.#^MEMBER6?check=MEMBER4^#
  }

  func f7(_ GA : [Gen]) {
    foo(1, b1 : GA.#^MEMBER7^#
  }

  func f8(_ GA : Gen) {
    foo(1, b1 : GA.IG.#^MEMBER8^#
  }

  func f9() {
    foo(1, b1 : GenGenerator(1).#^MEMBER9?check=MEMBER1^#
  }

  func f10(_ G: Gen) {
    foo(G.#^FARG3?check=MEMBER2^#
  }

  func f11(_ G: Gen) {
    bar(G.#^FARG4?check=MEMBER4^#
  }

  func f12(_ G1 : Gen, G2 : Gen) {
    G1.IntTaker(G2.#^FARG5?check=MEMBER2^#
  }

  func f13(_ G : Gen) {
    G.IntTaker(G.IG.#^FARG6^#
  }
}

// MEMBER1: Begin completions
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Convertible]: IntGen()[#Int#]; name=IntGen()
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: IntOpGen()[#Int?#]; name=IntOpGen()
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal:   StringGen()[#String#]; name=StringGen()
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal:   StringOpGen()[#String?#]; name=StringOpGen()
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: IntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]; name=IntTaker(i1: Int, i2: Int)
// MEMBER1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: StringTaker({#(s1): String#}, {#s2: String#})[#Void#]; name=StringTaker(s1: String, s2: String)
// MEMBER1-NOT: Keyword[try]/None: try; name=try
// MEMBER1-NOT: Keyword[try]/None: try!; name=try!
// MEMBER1-NOT: Keyword[try]/None: try?; name=try?
// MEMBER1-NOT: Keyword/None: await; name=await
// MEMBER1-NOT: Keyword[super]

// MEMBER2: Begin completions
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: IntGen()[#Int#]; name=IntGen()
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal:   IntOpGen()[#Int?#]; name=IntOpGen()
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal:   StringGen()[#String#]; name=StringGen()
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal:   StringOpGen()[#String?#]; name=StringOpGen()
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: IntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]; name=IntTaker(i1: Int, i2: Int)
// MEMBER2-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: StringTaker({#(s1): String#}, {#s2: String#})[#Void#]; name=StringTaker(s1: String, s2: String)

// MEMBER3: Begin completions
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal:   IntGen()[#Int#]; name=IntGen()
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal:   IntOpGen()[#Int?#]; name=IntOpGen()
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Convertible]: StringGen()[#String#]; name=StringGen()
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: StringOpGen()[#String?#]; name=StringOpGen()
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: IntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]; name=IntTaker(i1: Int, i2: Int)
// MEMBER3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: StringTaker({#(s1): String#}, {#s2: String#})[#Void#]; name=StringTaker(s1: String, s2: String)

// MEMBER4: Begin completions
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal:   IntGen()[#Int#]; name=IntGen()
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal:   IntOpGen()[#Int?#]; name=IntOpGen()
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: StringGen()[#String#]; name=StringGen()
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal:   StringOpGen()[#String?#]; name=StringOpGen()
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: IntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]; name=IntTaker(i1: Int, i2: Int)
// MEMBER4-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: StringTaker({#(s1): String#}, {#s2: String#})[#Void#]; name=StringTaker(s1: String, s2: String)

// MEMBER7: Begin completions
// MEMBER7-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: removeAll()[#Void#]; name=removeAll()
// MEMBER7-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem/TypeRelation[Invalid]: removeAll({#keepingCapacity: Bool#})[#Void#]; name=removeAll(keepingCapacity: Bool)
// MEMBER7-DAG: Decl[InstanceVar]/CurrNominal/IsSystem/TypeRelation[Convertible]: count[#Int#]; name=count
// MEMBER7-DAG: Decl[InstanceVar]/CurrNominal/IsSystem/TypeRelation[Convertible]: capacity[#Int#]; name=capacity

// MEMBER8: Begin completions
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Convertible]: InternalIntGen()[#Int#]; name=InternalIntGen()
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: InternalIntOpGen()[#Int?#]; name=InternalIntOpGen()
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal:   InternalStringGen()[#String#]; name=InternalStringGen()
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal:   InternalStringOpGen()[#String?#]; name=InternalStringOpGen()
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: InternalIntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]; name=InternalIntTaker(i1: Int, i2: Int)
// MEMBER8-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: InternalStringTaker({#(s1): String#}, {#s2: String#})[#Void#]; name=InternalStringTaker(s1: String, s2: String)

// FARG6: Begin completions
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Identical]: InternalIntGen()[#Int#]
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal:   InternalIntOpGen()[#Int?#]
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal:   InternalStringGen()[#String#]
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal:   InternalStringOpGen()[#String?#]
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: InternalIntTaker({#(i1): Int#}, {#i2: Int#})[#Void#]
// FARG6-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: InternalStringTaker({#(s1): String#}, {#s2: String#})[#Void#]

class C5 {}
class C6 : C5 {
  func f1() {
    foo(3, b: #^ARGSUPER?check=EXPECT-SUPER^#)
  }
}

// EXPECT-SUPER: Begin completions
// EXPECT-SUPER-DAG: Keyword[super]/CurrNominal: super[#C5#]; name=super

func firstArg(arg1 arg1: Int, arg2: Int) {}
func testArg1Name1() {
  firstArg(#^FIRST_ARG_NAME_1?check=FIRST_ARG_NAME_PATTERN^#
}
// FIRST_ARG_NAME_PATTERN: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#arg1: Int#}, {#arg2: Int#}[')'][#Void#];
func testArg2Name1() {
  firstArg(#^FIRST_ARG_NAME_2?check=FIRST_ARG_NAME_PATTERN^#)
}

func testArg2Name3() {
  firstArg(#^FIRST_ARG_NAME_3?check=FIRST_ARG_NAME_PATTERN^#,
}

func takeArray<T>(_ x: [T]) {}
struct TestBoundGeneric1 {
  let x: [Int]
  let y: [Int]
  func test1() {
    takeArray(self.#^BOUND_GENERIC_1_1?check=BOUND_GENERIC_1^#)
  }
  func test2() {
    takeArray(#^BOUND_GENERIC_1_2?check=BOUND_GENERIC_1^#)
  }
// BOUND_GENERIC_1: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]: x[#[Int]#];
// BOUND_GENERIC_1: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]: y[#[Int]#];
}

func whereConvertible<T>(lhs: T, rhs: T) where T: Collection {
  _ = zip(lhs, #^GENERIC_TO_GENERIC^#)
}
// GENERIC_TO_GENERIC: Begin completions
// GENERIC_TO_GENERIC: Decl[LocalVar]/Local: lhs[#Collection#]; name=lhs
// GENERIC_TO_GENERIC: Decl[LocalVar]/Local: rhs[#Collection#]; name=rhs
// GENERIC_TO_GENERIC: End completions

func emptyOverload() {}
func emptyOverload(foo foo: Int) {}
emptyOverload(foo: #^EMPTY_OVERLOAD_1?check=EMPTY_OVERLOAD^#)
struct EmptyOverload {
  init() {}
  init(foo: Int) {}
}
_ = EmptyOverload(foo: #^EMPTY_OVERLOAD_2?check=EMPTY_OVERLOAD^#)
// EMPTY_OVERLOAD: Begin completions
// EMPTY_OVERLOAD-DAG: Decl[GlobalVar]/Local/TypeRelation[Identical]: i2[#Int#];
// EMPTY_OVERLOAD-DAG: Decl[GlobalVar]/Local/TypeRelation[Identical]: i1[#Int#];
// EMPTY_OVERLOAD: End completions

public func fopen() -> TestBoundGeneric1! { fatalError() }
func other() {
  _ = fopen(#^CALLARG_IUO^#)
// CALLARG_IUO: Begin completions, 1 items
// CALLARG_IUO: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['('][')'][#TestBoundGeneric1!#]; name=
// CALLARG_IUO: End completions
}

class Foo { let x: Int }
class Bar {
  var collectionView: Foo!

  func foo() {
    self.collectionView? .#^BOUND_IUO?check=MEMBEROF_IUO^#x
    self.collectionView! .#^FORCED_IUO?check=MEMBEROF_IUO^#x
  }
  // MEMBEROF_IUO: Begin completions, 2 items
  // MEMBEROF_IUO: Keyword[self]/CurrNominal: self[#Foo#]; name=self
  // MEMBEROF_IUO: Decl[InstanceVar]/CurrNominal: x[#Int#]; name=x
  // MEMBEROF_IUO: End completions
}

func curry<T1, T2, R>(_ f: @escaping (T1, T2) -> R) -> (T1) -> (T2) -> R {
  return { t1 in { t2 in f(#^NESTED_CLOSURE^#, t2) } }
  // NESTED_CLOSURE: Begin completions
  // FIXME: Should be '/TypeRelation[Invalid]: t2[#T2#]'
  // NESTED_CLOSURE: Decl[LocalVar]/Local:               t2; name=t2
  // NESTED_CLOSURE: Decl[LocalVar]/Local:               t1[#T1#]; name=t1
}

func trailingClosureLocal(x: Int, fn: (Int) -> Void) {
  trailingClosureLocal(x: 1) { localArg in
    var localVar = 1
    if #^TRAILING_CLOSURE_LOCAL^#
  }
  // TRAILING_CLOSURE_LOCAL: Begin completions
  // TRAILING_CLOSURE_LOCAL-DAG: Decl[LocalVar]/Local: localArg[#Int#]; name=localArg
  // TRAILING_CLOSURE_LOCAL-DAG: Decl[LocalVar]/Local: localVar[#Int#]; name=localVar
}

func shuffled(_ x: Int ..., y: String = "", z: SimpleEnum = .foo) {}
func testTupleShuffle() {
  let _ = shuffled(1, 2, 3, 4, #^SHUFFLE_1^#, z: .foo)
  let _ = shuffled(1, 2, 3, y: #^SHUFFLE_2^#
  let _ = shuffled(z: .#^SHUFFLE_3^#)
}
// SHUFFLE_1: Begin completions
// SHUFFLE_1-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i1[#Int#]; name=i1
// SHUFFLE_1-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i2[#Int#]; name=i2

// SHUFFLE_2: Begin completions
// SHUFFLE_2-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s1[#String#]; name=s1
// SHUFFLE_2-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s2[#String#]; name=s2

// SHUFFLE_3: Begin completions, 4 items
// SHUFFLE_3-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     foo[#SimpleEnum#]; name=foo
// SHUFFLE_3-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     bar[#SimpleEnum#]; name=bar
// SHUFFLE_3-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     baz[#SimpleEnum#]; name=baz
// SHUFFLE_3-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:     hash({#(self): SimpleEnum#})[#(into: inout Hasher) -> Void#]; name=hash(self: SimpleEnum)


class HasSubscript {
  subscript(idx: Int) -> String {}
  subscript(idx: Int, default defaultValue: String) -> String {}
}
func testSubscript(obj: HasSubscript, intValue: Int, strValue: String) {
  let _ = obj[#^SUBSCRIPT_1^#
// SUBSCRIPT_1: Begin completions
// SUBSCRIPT_1-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i1[#Int#]; name=i1
// SUBSCRIPT_1-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: i2[#Int#]; name=i2
// SUBSCRIPT_1-DAG: Decl[GlobalVar]/CurrModule: s1[#String#]; name=s1
// SUBSCRIPT_1-DAG: Decl[GlobalVar]/CurrModule: s2[#String#]; name=s2
// SUBSCRIPT_1-DAG: Keyword[try]/None: try; name=try
// SUBSCRIPT_1-DAG: Keyword[try]/None: try!; name=try!
// SUBSCRIPT_1-DAG: Keyword[try]/None: try?; name=try?
// SUBSCRIPT_1-DAG: Keyword/None: await; name=await
// SUBSCRIPT_1-NOT: Keyword[super]

  let _ = obj[.#^SUBSCRIPT_1_DOT^#
// SUBSCRIPT_1_DOT: Begin completions
// SUBSCRIPT_1_DOT-NOT: i1
// SUBSCRIPT_1_DOT-NOT: s1
// SUBSCRIPT_1_DOT-DAG: Decl[StaticVar]/Super/Flair[ExprSpecific]/IsSystem/TypeRelation[Identical]: max[#Int#]; name=max
// SUBSCRIPT_1_DOT-DAG: Decl[StaticVar]/Super/Flair[ExprSpecific]/IsSystem/TypeRelation[Identical]: min[#Int#]; name=min

  let _ = obj[42, #^SUBSCRIPT_2^#
// SUBSCRIPT_2: Begin completions, 1 items
// SUBSCRIPT_2-NEXT: Pattern/Local/Flair[ArgLabels]: {#default: String#}[#String#];

  let _ = obj[42, .#^SUBSCRIPT_2_DOT^#
// Note: we still provide completions despite the missing label - there's a fixit to add it in later.
// SUBSCRIPT_2_DOT: Begin completions
// SUBSCRIPT_2_DOT: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Identical]: init()[#String#]; name=init()
// SUBSCRIPT_2_DOT: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Identical]: init({#(c): Character#})[#String#]; name=init(c: Character)

  let _ = obj[42, default: #^SUBSCRIPT_3^#
// SUBSCRIPT_3: Begin completions
// SUBSCRIPT_3-DAG: Decl[GlobalVar]/CurrModule: i1[#Int#]; name=i1
// SUBSCRIPT_3-DAG: Decl[GlobalVar]/CurrModule: i2[#Int#]; name=i2
// SUBSCRIPT_3-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s1[#String#]; name=s1
// SUBSCRIPT_3-DAG: Decl[GlobalVar]/CurrModule/TypeRelation[Identical]: s2[#String#]; name=s2
// SUBSCRIPT_3-DAG: Keyword[try]/None: try; name=try
// SUBSCRIPT_3-DAG: Keyword[try]/None: try!; name=try!
// SUBSCRIPT_3-DAG: Keyword[try]/None: try?; name=try?
// SUBSCRIPT_3-DAG: Keyword/None: await; name=await
// SUBSCRIPT_3-NOT: Keyword[super]

  let _ = obj[42, default: .#^SUBSCRIPT_3_DOT^#
// SUBSCRIPT_3_DOT: Begin completions
// SUBSCRIPT_3_DOT-NOT: i1
// SUBSCRIPT_3_DOT-NOT: s1
// SUBSCRIPT_3_DOT-DAG: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Identical]: init()[#String#]; name=init()
// SUBSCRIPT_3_DOT-DAG: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Identical]: init({#(c): Character#})[#String#]; name=init(c: Character)

}

func testNestedContext() {
  func foo(_ x: Int) {}
  func bar(_ y: TypeInvalid) -> Int {}

  let _ = foo(bar(#^ERRORCONTEXT_NESTED_1^#))
// ERRORCONTEXT_NESTED_1: Begin completions
// ERRORCONTEXT_NESTED_1-DAG: Decl[GlobalVar]/CurrModule: i1[#Int#]; name=i1
// ERRORCONTEXT_NESTED_1-DAG: Decl[GlobalVar]/CurrModule: i2[#Int#]; name=i2
// ERRORCONTEXT_NESTED_1-NOT: TypeRelation[Identical]

  for _ in [bar(#^ERRORCONTEXT_NESTED_2?check=ERRORCONTEXT_NESTED_1^#)] {}
// Same as ERRORCONTEXT_NESTED_1.
}

class TestImplicitlyCurriedSelf {
  func foo(x: Int) { }
  func foo(arg: Int, optArg: Int) { }

  static func test() {
    foo(#^CURRIED_SELF_1^#
    func sync();
    self.foo(#^CURRIED_SELF_2?check=CURRIED_SELF_1^#
    func sync();
    TestImplicitlyCurriedSelf.foo(#^CURRIED_SELF_3?check=CURRIED_SELF_1^#

// CURRIED_SELF_1: Begin completions, 2 items
// CURRIED_SELF_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(self): TestImplicitlyCurriedSelf#}[')'][#(Int) -> ()#]{{; name=.+$}}
// CURRIED_SELF_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(self): TestImplicitlyCurriedSelf#}[')'][#(Int, Int) -> ()#]{{; name=.+$}}
// CURRIED_SELF_1: End completions
  }
}

class TestStaticMemberCall {
  static func create1(arg1: Int) -> TestStaticMemberCall {
    return TestStaticMemberCall()
  }
  static func create2(_ arg1: Int, arg2: Int = 0, arg3: Int = 1, arg4: Int = 2) -> TestStaticMemberCall {
    return TestStaticMemberCall()
  }
  static func createOverloaded(arg1: Int) -> TestStaticMemberCall { TestStaticMemberCall() }
  static func createOverloaded(arg1: String) -> String { arg1 }
}
func testStaticMemberCall() {
  let _ = TestStaticMemberCall.create1(#^STATIC_METHOD_AFTERPAREN_1^#)
// STATIC_METHOD_AFTERPAREN_1: Begin completions, 1 items
// STATIC_METHOD_AFTERPAREN_1: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]:     ['(']{#arg1: Int#}[')'][#TestStaticMemberCall#]; name=arg1: Int
// STATIC_METHOD_AFTERPAREN_1: End completions

  let _ = TestStaticMemberCall.create2(#^STATIC_METHOD_AFTERPAREN_2^#)
// STATIC_METHOD_AFTERPAREN_2: Begin completions
// STATIC_METHOD_AFTERPAREN_2-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#(arg1): Int#}[')'][#TestStaticMemberCall#];
// STATIC_METHOD_AFTERPAREN_2-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#(arg1): Int#}, {#arg2: Int#}, {#arg3: Int#}, {#arg4: Int#}[')'][#TestStaticMemberCall#];
// STATIC_METHOD_AFTERPAREN_2-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem/TypeRelation[Identical]: Int[#Int#];
// STATIC_METHOD_AFTERPAREN_2-DAG: Literal[Integer]/None/TypeRelation[Identical]: 0[#Int#];
// STATIC_METHOD_AFTERPAREN_2: End completions

  let _ = TestStaticMemberCall.create2(1, #^STATIC_METHOD_SECOND^#)
// STATIC_METHOD_SECOND: Begin completions, 3 items
// STATIC_METHOD_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg2: Int#}[#Int#];
// STATIC_METHOD_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg3: Int#}[#Int#];
// STATIC_METHOD_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg4: Int#}[#Int#];
// STATIC_METHOD_SECOND: End completions

  let _ = TestStaticMemberCall.create2(1, arg3: 2, #^STATIC_METHOD_SKIPPED^#)
// STATIC_METHOD_SKIPPED: Begin completions, 1 item
// STATIC_METHOD_SKIPPED: Pattern/Local/Flair[ArgLabels]: {#arg4: Int#}[#Int#];
// STATIC_METHOD_SKIPPED: End completions

  let _ = TestStaticMemberCall.createOverloaded(#^STATIC_METHOD_OVERLOADED^#)
// STATIC_METHOD_OVERLOADED: Begin completions, 2 items
// STATIC_METHOD_OVERLOADED-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]:     ['(']{#arg1: Int#}[')'][#TestStaticMemberCall#]; name=arg1: Int
// STATIC_METHOD_OVERLOADED-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]:     ['(']{#arg1: String#}[')'][#String#]; name=arg1: String
// STATIC_METHOD_OVERLOADED: End completions
}
func testImplicitMember() {
  let _: TestStaticMemberCall = .create1(#^IMPLICIT_MEMBER_AFTERPAREN_1^#)
// IMPLICIT_MEMBER_AFTERPAREN_1: Begin completions, 1 items
// IMPLICIT_MEMBER_AFTERPAREN_1: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#arg1: Int#}[')'][#TestStaticMemberCall#]; name=arg1: Int
// IMPLICIT_MEMBER_AFTERPAREN_1: End completions

  let _: TestStaticMemberCall = .create2(#^IMPLICIT_MEMBER_AFTERPAREN_2^#)
// IMPLICIT_MEMBER_AFTERPAREN_2: Begin completions
// IMPLICIT_MEMBER_AFTERPAREN_2-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#(arg1): Int#}[')'][#TestStaticMemberCall#];
// IMPLICIT_MEMBER_AFTERPAREN_2-DAG: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#(arg1): Int#}, {#arg2: Int#}, {#arg3: Int#}, {#arg4: Int#}[')'][#TestStaticMemberCall#];
// IMPLICIT_MEMBER_AFTERPAREN_2-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem/TypeRelation[Identical]: Int[#Int#];
// IMPLICIT_MEMBER_AFTERPAREN_2-DAG: Literal[Integer]/None/TypeRelation[Identical]: 0[#Int#];
// IMPLICIT_MEMBER_AFTERPAREN_2: End completions

  let _: TestStaticMemberCall? = .create1(#^IMPLICIT_MEMBER_AFTERPAREN_3^#)
// IMPLICIT_MEMBER_AFTERPAREN_3: Begin completions, 1 items
// IMPLICIT_MEMBER_AFTERPAREN_3: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#arg1: Int#}[')'][#TestStaticMemberCall#]; name=arg1: Int
// IMPLICIT_MEMBER_AFTERPAREN_3: End completions

  let _: TestStaticMemberCall = .create2(1, #^IMPLICIT_MEMBER_SECOND^#)
// IMPLICIT_MEMBER_SECOND: Begin completions, 3 items
// IMPLICIT_MEMBER_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg2: Int#}[#Int#];
// IMPLICIT_MEMBER_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg3: Int#}[#Int#];
// IMPLICIT_MEMBER_SECOND: Pattern/Local/Flair[ArgLabels]: {#arg4: Int#}[#Int#];
// IMPLICIT_MEMBER_SECOND: End completions

  let _: TestStaticMemberCall = .create2(1, arg3: 2, #^IMPLICIT_MEMBER_SKIPPED^#)
// IMPLICIT_MEMBER_SKIPPED: Begin completions, 1 item
// IMPLICIT_MEMBER_SKIPPED: Pattern/Local/Flair[ArgLabels]: {#arg4: Int#}[#Int#];
// IMPLICIT_MEMBER_SKIPPED: End completions

  let _: TestStaticMemberCall = .createOverloaded(#^IMPLICIT_MEMBER_OVERLOADED^#)
// IMPLICIT_MEMBER_OVERLOADED: Begin completions, 2 items
// IMPLICIT_MEMBER_OVERLOADED: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Identical]: ['(']{#arg1: Int#}[')'][#TestStaticMemberCall#]; name=arg1: Int
// IMPLICIT_MEMBER_OVERLOADED: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]:     ['(']{#arg1: String#}[')'][#String#]; name=arg1: String
// IMPLICIT_MEMBER_OVERLOADED: End completions
}
func testImplicitMemberInArrayLiteral() {
  struct Receiver {
    init(_: [TestStaticMemberCall]) {}
    init(arg1: Int, arg2: [TestStaticMemberCall]) {}
  }

  Receiver([
    .create1(x: 1),
    .create1(#^IMPLICIT_MEMBER_ARRAY_1_AFTERPAREN_1?check=IMPLICIT_MEMBER_AFTERPAREN_1^#),
    // Same as IMPLICIT_MEMBER_AFTERPAREN_1.
  ])
  Receiver([
    .create1(x: 1),
    .create2(#^IMPLICIT_MEMBER_ARRAY_1_AFTERPAREN_2?check=IMPLICIT_MEMBER_AFTERPAREN_2^#),
    // Same as IMPLICIT_MEMBER_AFTERPAREN_2.
  ])
  Receiver([
    .create1(x: 1),
    .create2(1, #^IMPLICIT_MEMBER_ARRAY_1_SECOND?check=IMPLICIT_MEMBER_SECOND^#
    // Same as IMPLICIT_MEMBER_SECOND.
  ])
  Receiver(arg1: 12, arg2: [
    .create2(1, arg3: 2, #^IMPLICIT_MEMBER_ARRAY_1_SKIPPED?check=IMPLICIT_MEMBER_SKIPPED^#
    // Same as IMPLICIT_MEMBER_SKIPPED.
    .create1(x: 12)
  ])
  Receiver(arg1: 12, arg2: [
    .create1(x: 12),
    .createOverloaded(#^IMPLICIT_MEMBER_ARRAY_1_OVERLOADED?check=IMPLICIT_MEMBER_OVERLOADED^#)
    // Same as IMPLICIT_MEMBER_OVERLOADED.
  ])
  let _: [TestStaticMemberCall] = [
    .create1(#^IMPLICIT_MEMBER_ARRAY_2_AFTERPAREN_1?check=IMPLICIT_MEMBER_AFTERPAREN_1^#),
    // Same as IMPLICIT_MEMBER_AFTERPAREN_1.
    .create2(#^IMPLICIT_MEMBER_ARRAY_2_AFTERPAREN_2?check=IMPLICIT_MEMBER_AFTERPAREN_2^#),
    // Same as IMPLICIT_MEMBER_AFTERPAREN_2.
    .create2(1, #^IMPLICIT_MEMBER_ARRAY_2_SECOND?check=IMPLICIT_MEMBER_SECOND^#),
    // Same as IMPLICIT_MEMBER_SECOND.
    .create2(1, arg3: 2, #^IMPLICIT_MEMBER_ARRAY_2_SKIPPED?check=IMPLICIT_MEMBER_SKIPPED^#),
    // Same as IMPLICIT_MEMBER_SKIPPED.
    .createOverloaded(#^IMPLICIT_MEMBER_ARRAY_2_OVERLOADED?check=IMPLICIT_MEMBER_OVERLOADED^#),
    // Same as IMPLICIT_MEMBER_OVERLOADED
  ]
}

struct Wrap<T> {
  func method<U>(_ fn: (T) -> U) -> Wrap<U> {}
}
func testGenricMethodOnGenericOfArchetype<Wrapped>(value: Wrap<Wrapped>) {
  value.method(#^ARCHETYPE_GENERIC_1^#)
// ARCHETYPE_GENERIC_1: Begin completions
// ARCHETYPE_GENERIC_1: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#(fn): (Wrapped) -> U##(Wrapped) -> U#}[')'][#Wrap<U>#];
}

struct TestHasErrorAutoclosureParam {
  func hasErrorAutoclosureParam(value: @autoclosure () -> Value) {
    fatalError()
  }
  func test() {
    hasErrorAutoclosureParam(#^PARAM_WITH_ERROR_AUTOCLOSURE^#
// PARAM_WITH_ERROR_AUTOCLOSURE: Begin completions, 1 items
// PARAM_WITH_ERROR_AUTOCLOSURE: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#value: <<error type>>#}[')'][#Void#];
// PARAM_WITH_ERROR_AUTOCLOSURE: End completions
  }
}

struct MyType<T> {
  init(arg1: String, arg2: T) {}
  func overloaded() {}
  func overloaded(_ int: Int) {}
  func overloaded(name: String, value: String) {}
}
func testTypecheckedOverloaded<T>(value: MyType<T>) {
  value.overloaded(#^TYPECHECKED_OVERLOADED^#)
// TYPECHECKED_OVERLOADED: Begin completions
// TYPECHECKED_OVERLOADED-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['('][')'][#Void#];
// TYPECHECKED_OVERLOADED-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#(int): Int#}[')'][#Void#];
// TYPECHECKED_OVERLOADED-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]:   ['(']{#name: String#}, {#value: String#}[')'][#Void#];
// TYPECHECKED_OVERLOADED: End completions
}

extension MyType where T == Int {
  init(_ intVal: T) {}
}
func testTypecheckedTypeExpr() {
  MyType(#^TYPECHECKED_TYPEEXPR^#
}
// TYPECHECKED_TYPEEXPR: Begin completions
// TYPECHECKED_TYPEEXPR: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#arg1: String#}, {#arg2: _#}[')'][#MyType<_>#]; name=arg1: String, arg2: _
// TYPECHECKED_TYPEEXPR: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#(intVal): Int#}[')'][#MyType<Int>#]; name=intVal: Int
// TYPECHECKED_TYPEEXPR: End completions

func testPamrameterFlags(_: Int, inoutArg: inout Int, autoclosureArg: @autoclosure () -> Int, iuoArg: Int!, variadicArg: Int...) {
  var intVal = 1
  testPamrameterFlags(intVal, #^ARG_PARAMFLAG_INOUT^#)
// ARG_PARAMFLAG_INOUT: Begin completions, 1 items
// ARG_PARAMFLAG_INOUT-DAG: Pattern/Local/Flair[ArgLabels]: {#inoutArg: &Int#}[#inout Int#]; name=inoutArg:
// ARG_PARAMFLAG_INOUT: End completions

  testPamrameterFlags(intVal, inoutArg: &intVal, #^ARG_PARAMFLAG_AUTOCLOSURE^#)
// ARG_PARAMFLAG_AUTOCLOSURE: Begin completions, 1 items
// ARG_PARAMFLAG_AUTOCLOSURE-DAG: Pattern/Local/Flair[ArgLabels]: {#autoclosureArg: Int#}[#Int#];
// ARG_PARAMFLAG_AUTOCLOSURE: End completions

  testPamrameterFlags(intVal, inoutArg: &intVal, autoclosureArg: intVal, #^ARG_PARAMFLAG_IUO^#)
// ARG_PARAMFLAG_IUO: Begin completions, 1 items
// ARG_PARAMFLAG_IUO-DAG: Pattern/Local/Flair[ArgLabels]: {#iuoArg: Int?#}[#Int?#];
// ARG_PARAMFLAG_IUO: End completions

  testPamrameterFlags(intVal, inoutArg: &intVal, autoclosureArg: intVal, iuoArg: intVal, #^ARG_PARAMFLAG_VARIADIC^#)
// ARG_PARAMFLAG_VARIADIC: Begin completions, 1 items
// ARG_PARAMFLAG_VARIADIC-DAG: Pattern/Local/Flair[ArgLabels]: {#variadicArg: Int...#}[#Int#];
// ARG_PARAMFLAG_VARIADIC: End completions
}

func testTupleElement(arg: (SimpleEnum, SimpleEnum)) {
  testTupleElement(arg: (.foo, .#^TUPLEELEM_1^#))
// TUPLEELEM_1: Begin completions, 4 items
// TUPLEELEM_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     foo[#SimpleEnum#]; name=foo
// TUPLEELEM_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     bar[#SimpleEnum#]; name=bar
// TUPLEELEM_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     baz[#SimpleEnum#]; name=baz
// TUPLEELEM_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:     hash({#(self): SimpleEnum#})[#(into: inout Hasher) -> Void#]; name=hash(self: SimpleEnum)
// TUPLEELEM_1: End completions
  testTupleElement(arg: (.foo, .bar, .#^TUPLEELEM_2^#))
// TUPLEELEM_2-NOT: Begin completions
}

func testKeyPathThunkInBase() {
    struct TestKP {
        var value: Int { 1 }
    }
    struct TestKPResult {
        func testFunc(_ arg: SimpleEnum) {}
    }
    func foo(_ fn: (TestKP) -> Int) -> TestKPResult { TestKPResult() }

    foo(\.value).testFunc(.#^KEYPATH_THUNK_BASE^#)
// KEYPATH_THUNK_BASE: Begin completions, 4 items
// KEYPATH_THUNK_BASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     foo[#SimpleEnum#]; name=foo
// KEYPATH_THUNK_BASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     bar[#SimpleEnum#]; name=bar
// KEYPATH_THUNK_BASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]:     baz[#SimpleEnum#]; name=baz
// KEYPATH_THUNK_BASE-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:     hash({#(self): SimpleEnum#})[#(into: inout Hasher) -> Void#]; name=hash(self: SimpleEnum)
// KEYPATH_THUNK_BASE: End completions
}

func testVariadic(_ arg: Any..., option1: Int = 0, option2: String = 1) {
    testVariadic(#^VARIADIC_1^#)
// VARIADIC_1: Begin completions
// VARIADIC_1-DAG: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#(arg): Any...#}, {#option1: Int#}, {#option2: String#}[')'][#Void#];
// VARIADIC_1-DAG: Decl[GlobalVar]/CurrModule:    i1[#Int#];
// VARIADIC_1: End completions
    testVariadic(1, #^VARIADIC_2^#)
// VARIADIC_2: Begin completions
// VARIADIC_2-DAG: Pattern/Local/Flair[ArgLabels]:          {#option1: Int#}[#Int#];
// VARIADIC_2-DAG: Pattern/Local/Flair[ArgLabels]:          {#option2: String#}[#String#];
// VARIADIC_2-DAG: Decl[GlobalVar]/CurrModule:    i1[#Int#];
// VARIADIC_2: End completions
    testVariadic(1, 2, #^VARIADIC_3?check=VARIADIC_2^#)
// Same as VARIADIC_2.
}

func testLabelsInSelfDotInit() {
  class Foo {
    init(a: Int, b: Int) {}
    convenience init() {
      self.init(a: 1, #^LABEL_IN_SELF_DOT_INIT^#)
// LABEL_IN_SELF_DOT_INIT: Begin completions, 1 item
// LABEL_IN_SELF_DOT_INIT-DAG: Pattern/Local/Flair[ArgLabels]:               {#b: Int#}[#Int#]
// LABEL_IN_SELF_DOT_INIT: End completions
    }
  }
}

func testMissingRequiredParameter() {
  class C {
    func foo(x: Int, y: Int, z: Int)  {}
  }
  func test(c: C) {
    c.foo(y: 1, #^MISSING_REQUIRED_PARAM^#)
// MISSING_REQUIRED_PARAM: Begin completions, 1 item
// MISSING_REQUIRED_PARAM-DAG: Pattern/Local/Flair[ArgLabels]:               {#z: Int#}[#Int#]
// MISSING_REQUIRED_PARAM: End completions
  }
}

func testAfterVariadic() {
  class C {
    func foo(x: Int..., y: Int, z: Int)  {}
  }
  func test(c: C) {
    c.foo(x: 10, 20, 30, y: 40, #^NAMED_PARAMETER_WITH_LEADING_VARIADIC^#)
// NAMED_PARAMETER_WITH_LEADING_VARIADIC: Begin completions, 1 item
// NAMED_PARAMETER_WITH_LEADING_VARIADIC-DAG: Pattern/Local/Flair[ArgLabels]:               {#z: Int#}[#Int#]
// NAMED_PARAMETER_WITH_LEADING_VARIADIC: End completions
  }
}

func testClosurePlaceholderContainsInternalParameterNamesIfPresentInSiganture() {
  func sort(callback: (_ left: Int, _ right: Int) -> Bool) {}
  sort(#^CLOSURE_PARAM_WITH_INTERNAL_NAME^#)
// CLOSURE_PARAM_WITH_INTERNAL_NAME: Begin completions, 1 item
// CLOSURE_PARAM_WITH_INTERNAL_NAME-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]:           ['(']{#callback: (Int, Int) -> Bool##(_ left: Int, _ right: Int) -> Bool#}[')'][#Void#];
// CLOSURE_PARAM_WITH_INTERNAL_NAME: End completions

  func sortWithParensAroundClosureType(callback: ((_ left: Int, _ right: Int) -> Bool)) {}
  sortWithParensAroundClosureType(#^CLOSURE_PARAM_WITH_PARENS^#)
// CLOSURE_PARAM_WITH_PARENS: Begin completions, 1 item
// CLOSURE_PARAM_WITH_PARENS-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]:           ['(']{#callback: ((Int, Int) -> Bool)##(_ left: Int, _ right: Int) -> Bool#}[')'][#Void#];
// CLOSURE_PARAM_WITH_PARENS: End completions

  func sortWithOptionalClosureType(callback: ((_ left: Int, _ right: Int) -> Bool)?) {}
  sortWithOptionalClosureType(#^OPTIONAL_CLOSURE_PARAM^#)
// OPTIONAL_CLOSURE_PARAM: Begin completions, 1 item
// OPTIONAL_CLOSURE_PARAM-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]:           ['(']{#callback: ((Int, Int) -> Bool)?##(_ left: Int, _ right: Int) -> Bool#}[')'][#Void#];
// OPTIONAL_CLOSURE_PARAM: End completions

  func sortWithEscapingClosureType(callback: @escaping (_ left: Int, _ right: Int) -> Bool) {}
  sortWithEscapingClosureType(#^ESCAPING_OPTIONAL_CLOSURE_PARAM^#)
// ESCAPING_OPTIONAL_CLOSURE_PARAM: Begin completions, 1 item
// ESCAPING_OPTIONAL_CLOSURE_PARAM-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]:           ['(']{#callback: (Int, Int) -> Bool##(_ left: Int, _ right: Int) -> Bool#}[')'][#Void#];
// ESCAPING_OPTIONAL_CLOSURE_PARAM: End completions
}

func testClosurePlaceholderPrintsTypesOnlyIfNoInternalParameterNamesExist() {
  func sort(callback: (Int, Int) -> Bool) {}
  sort(#^COMPLETE_CLOSURE_PARAM_WITHOUT_INTERNAL_NAMES^#)
// COMPLETE_CLOSURE_PARAM_WITHOUT_INTERNAL_NAMES: Begin completions, 1 item
// COMPLETE_CLOSURE_PARAM_WITHOUT_INTERNAL_NAMES-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]:           ['(']{#callback: (Int, Int) -> Bool##(Int, Int) -> Bool#}[')'][#Void#];
// COMPLETE_CLOSURE_PARAM_WITHOUT_INTERNAL_NAMES: End completions
}

func testCompleteLabelAfterVararg() {
  enum Foo {
    case bar
  }
  enum Baz {
    case bazCase
  }

  struct Rdar76355192 {
    func test(_: String, xArg: Foo..., yArg: Foo..., zArg: Foo...) {}
  }

  private func test(value: Rdar76355192) {
    value.test("test", xArg: #^COMPLETE_AFTER_VARARG^#)
    // COMPLETE_AFTER_VARARG: Begin completions
    // COMPLETE_AFTER_VARARG-NOT: Pattern/Local/Flair[ArgLabels]:               {#yArg: Foo...#}[#Foo#];
    // COMPLETE_AFTER_VARARG-NOT: Pattern/Local/Flair[ArgLabels]:               {#zArg: Foo...#}[#Foo#];
    // COMPLETE_AFTER_VARARG: End completions
    value.test("test", xArg: .bar, #^COMPLETE_AFTER_VARARG_WITH_PREV_PARAM^#)
    // COMPLETE_AFTER_VARARG_WITH_PREV_PARAM: Begin completions
    // COMPLETE_AFTER_VARARG_WITH_PREV_PARAM-DAG: Pattern/Local/Flair[ArgLabels]:               {#yArg: Foo...#}[#Foo#];
    // COMPLETE_AFTER_VARARG_WITH_PREV_PARAM-DAG: Pattern/Local/Flair[ArgLabels]:               {#zArg: Foo...#}[#Foo#];
    // COMPLETE_AFTER_VARARG_WITH_PREV_PARAM: End completions
    value.test("test", xArg: .bar, .#^COMPLETE_MEMBER_IN_VARARG^#)
    // COMPLETE_MEMBER_IN_VARARG: Begin completions, 2 items
    // COMPLETE_MEMBER_IN_VARARG-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bar[#Foo#];
    // COMPLETE_MEMBER_IN_VARARG-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): Foo#})[#(into: inout Hasher) -> Void#];
    // COMPLETE_MEMBER_IN_VARARG: End completions
  }

  struct Sr14515 {
    func test(_: Foo..., yArg: Baz) {}
  }

  private func testSr14515(value: Sr14515, foo: Foo, baz: Baz) {
    value.test(foo, #^COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG^#)
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG: Begin completions
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG-DAG: Decl[LocalVar]/Local/TypeRelation[Identical]: foo[#Foo#];
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG-DAG: Pattern/Local/Flair[ArgLabels]:               {#yArg: Baz#}[#Baz#];
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG: End completions

    // The leading dot completion tests that have picked the right type for the argument
    value.test(foo, .#^COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG_DOT^#)
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG_DOT: Begin completions, 2 items
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG_DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bar[#Foo#];
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG_DOT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): Foo#})[#(into: inout Hasher) -> Void#];
    // COMPLETE_VARARG_FOLLOWED_BY_NORMAL_ARG_DOT: End completions

    value.test(foo, yArg: #^COMPLETE_ARG_AFTER_VARARG^#)
    // COMPLETE_ARG_AFTER_VARARG: Begin completions
    // COMPLETE_ARG_AFTER_VARARG-DAG: Decl[LocalVar]/Local/TypeRelation[Identical]: baz[#Baz#];
    // COMPLETE_ARG_AFTER_VARARG-NOT: Pattern/Local/Flair[ArgLabels]:               {#yArg: Baz#}[#Baz#];
    // COMPLETE_ARG_AFTER_VARARG: End completions

    value.test(foo, yArg: .#^COMPLETE_ARG_AFTER_VARARG_DOT^#)
    // COMPLETE_ARG_AFTER_VARARG_DOT: Begin completions, 2 items
    // COMPLETE_ARG_AFTER_VARARG_DOT-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Identical]: bazCase[#Baz#];
    // COMPLETE_ARG_AFTER_VARARG_DOT-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: hash({#(self): Baz#})[#(into: inout Hasher) -> Void#];
    // COMPLETE_ARG_AFTER_VARARG_DOT: End completions
  }
}

func testGenericConstructor() {
  public struct TextField<Label> {
    init(label: String, text: String) {}
  }

  _ = TextField(label: "Encoded", #^GENERIC_INITIALIZER^#)
// GENERIC_INITIALIZER: Begin completions, 1 item
// GENERIC_INITIALIZER-DAG: Pattern/Local/Flair[ArgLabels]:               {#text: String#}[#String#]
// GENERIC_INITIALIZER: End completions
}

struct Rdar77867723 {
  enum Horizontal { case east, west }
  enum Vertical { case up, down }
  func fn(aaa: Horizontal = .east, bbb: Vertical = .up) {}
  func fn(ccc: Vertical = .up, ddd: Horizontal = .west) {}
  func test1 {
    self.fn(ccc: .up, #^OVERLOAD_LABEL1^#)
// OVERLOAD_LABEL1: Begin completions, 1 items
// OVERLOAD_LABEL1-DAG: Pattern/Local/Flair[ArgLabels]:               {#ddd: Horizontal#}[#Horizontal#];
// OVERLOAD_LABEL1: End completions
  }
  func test2 {
    self.fn(eee: .up, #^OVERLOAD_LABEL2^#)
// OVERLOAD_LABEL2: Begin completions, 4 items
// OVERLOAD_LABEL2-DAG: Pattern/Local/Flair[ArgLabels]:               {#aaa: Horizontal#}[#Horizontal#];
// OVERLOAD_LABEL2-DAG: Pattern/Local/Flair[ArgLabels]:               {#bbb: Vertical#}[#Vertical#];
// OVERLOAD_LABEL2-DAG: Pattern/Local/Flair[ArgLabels]:               {#ccc: Vertical#}[#Vertical#];
// OVERLOAD_LABEL2-DAG: Pattern/Local/Flair[ArgLabels]:               {#ddd: Horizontal#}[#Horizontal#];
// OVERLOAD_LABEL2: End completions
  }
}
