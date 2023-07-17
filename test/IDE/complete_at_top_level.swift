// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// NORESULT: Token

// Test code completion in top-level code.
//
// This test is not meant to test that we can correctly form all kinds of
// completion results in general; that should be tested elsewhere.

var topLevelVar3 = #^TOP_LEVEL_VAR_INIT_3?check=TOP_LEVEL_VAR_INIT_3_NEGATIVE^#
// TOP_LEVEL_VAR_INIT_3_NEGATIVE-NOT: topLevelVar3

struct FooStruct {
  var instanceVar = 0

  func instanceFunc(_ a: Int) {}
  // Add more stuff as needed.
}

var fooObject : FooStruct

func fooFunc1() {}
func fooFunc2(_ a: Int, _ b: Double) {}

func erroneous1(_ x: Undeclared) {}

// FIXME: Hides all other string interpolation completion.
//extension DefaultStringInterpolation {
//  mutating func appendInterpolation(interpolate: Double) {}
//}

//===--- Test code completions of expressions that can be typechecked.

// Although the parser can recover in most of these test cases, we resync it
// anyway to ensure that there parser recovery does not interfere with code
// completion.
func resyncParser1() {}

fooObject#^TYPE_CHECKED_EXPR_1^#
// TYPE_CHECKED_EXPR_1-DAG: Decl[InstanceVar]/CurrNominal:      .instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_1-DAG: Decl[InstanceMethod]/CurrNominal:   .instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_1-DAG: BuiltinOperator/None:               = {#FooStruct#}{{; name=.+$}}
// TYPE_CHECKED_EXPR_1-DAG: Keyword[self]/CurrNominal:          .self[#FooStruct#]{{; name=.+$}}

func resyncParser2() {}

// Test that we can code complete after a top-level var decl.
var _tmpVar1 : FooStruct

fooObject#^TYPE_CHECKED_EXPR_2^#
// TYPE_CHECKED_EXPR_2-DAG: Decl[InstanceVar]/CurrNominal: .instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_2-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_2-DAG: BuiltinOperator/None:                     = {#FooStruct#}{{; name=.+$}}
// TYPE_CHECKED_EXPR_2-DAG: Keyword[self]/CurrNominal: .self[#FooStruct#]{{; name=.+$}}

func resyncParser3() {}

fooObject#^TYPE_CHECKED_EXPR_3^#.bar
// TYPE_CHECKED_EXPR_3-DAG: Decl[InstanceVar]/CurrNominal: .instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_3-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_3-DAG: BuiltinOperator/None:                     = {#FooStruct#}{{; name=.+$}}
// TYPE_CHECKED_EXPR_3-DAG: Keyword[self]/CurrNominal: .self[#FooStruct#]{{; name=.+$}}

func resyncParser4() {}

fooObject.#^TYPE_CHECKED_EXPR_4^#
// TYPE_CHECKED_EXPR_4-DAG: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// TYPE_CHECKED_EXPR_4-DAG: Decl[InstanceVar]/CurrNominal: instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_4-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}

func resyncParser5() {}

fooObject.#^TYPE_CHECKED_EXPR_5^#.bar
// TYPE_CHECKED_EXPR_5-DAG: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// TYPE_CHECKED_EXPR_5-DAG: Decl[InstanceVar]/CurrNominal: instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_5-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}

func resyncParser6() {}

fooObject.instanceFunc(#^TYPE_CHECKED_EXPR_6?check=PLAIN_TOP_LEVEL^#

func resyncParser6() {}

fooObject.is#^TYPE_CHECKED_EXPR_KW_1?check=NORESULT^#

func resyncParser7() {}

// We have an error in the initializer here, but the type is explicitly written
// in the source.
var fooObjectWithErrorInInit : FooStruct = unknown_var

fooObjectWithErrorInInit.#^TYPE_CHECKED_EXPR_WITH_ERROR_IN_INIT_1^#
// TYPE_CHECKED_EXPR_WITH_ERROR_IN_INIT_1-DAG: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// TYPE_CHECKED_EXPR_WITH_ERROR_IN_INIT_1-DAG: Decl[InstanceVar]/CurrNominal: instanceVar[#Int#]{{; name=.+$}}
// TYPE_CHECKED_EXPR_WITH_ERROR_IN_INIT_1-DAG: Decl[InstanceMethod]/CurrNominal: instanceFunc({#(a): Int#})[#Void#]{{; name=.+$}}

func resyncParser6a() {}

var topLevelVar1 = #^TOP_LEVEL_VAR_INIT_1?check=TOP_LEVEL_VAR_INIT_1;check=TOP_LEVEL_VAR_INIT_1_NEGATIVE;check=NEGATIVE^#
// TOP_LEVEL_VAR_INIT_1-DAG: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_1-DAG: Decl[FreeFunction]/CurrModule: fooFunc1()[#Void#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_1-DAG: Decl[GlobalVar]/Local: fooObject[#FooStruct#]{{; name=.+$}}

// Check that the variable itself does not show up.
// TOP_LEVEL_VAR_INIT_1_NEGATIVE-NOT: topLevelVar1

func resyncParser7() {}

var topLevelVar2 = FooStruct#^TOP_LEVEL_VAR_INIT_2^#
// TOP_LEVEL_VAR_INIT_2-DAG: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#(self): FooStruct#})[#(Int) -> Void#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ({#instanceVar: Int#})[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_2-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ()[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_VAR_INIT_2-DAG: Keyword[self]/CurrNominal: .self[#FooStruct.Type#]; name=self
// TOP_LEVEL_VAR_INIT_2-DAG: Keyword/CurrNominal:       .Type[#FooStruct.Type#]; name=Type

func resyncParser8() {}

#^PLAIN_TOP_LEVEL_1?check=PLAIN_TOP_LEVEL;check=PLAIN_TOP_LEVEL_NO_DUPLICATES;check=NEGATIVE^#
// PLAIN_TOP_LEVEL-DAG: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}
// PLAIN_TOP_LEVEL-DAG: Decl[GlobalVar]/Local: fooObject[#FooStruct#]{{; name=.+$}}

// PLAIN_TOP_LEVEL_NO_DUPLICATES-DAG: Decl[FreeFunction]/CurrModule: fooFunc1()[#Void#]{{; name=.+$}}
// PLAIN_TOP_LEVEL_NO_DUPLICATES-DAG: Decl[FreeFunction]/CurrModule: fooFunc2({#(a): Int#}, {#(b): Double#})[#Void#]{{; name=.+$}}
// PLAIN_TOP_LEVEL_NO_DUPLICATES-NOT: fooFunc1
// PLAIN_TOP_LEVEL_NO_DUPLICATES-NOT: fooFunc2

func resyncParser9() {}

// Test that we can code complete immediately after a decl with a syntax error.
func _tmpFuncWithSyntaxError() { if return }

#^PLAIN_TOP_LEVEL_2?check=PLAIN_TOP_LEVEL^#

func resyncParser10() {}

_ = {
  #^TOP_LEVEL_CLOSURE_1^#
}()
// TOP_LEVEL_CLOSURE_1-DAG: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_CLOSURE_1-DAG: Decl[FreeFunction]/CurrModule: fooFunc1()[#Void#]{{; name=.+$}}
// TOP_LEVEL_CLOSURE_1-DAG: Decl[GlobalVar]/Local: fooObject[#FooStruct#]{{; name=.+$}}

func resyncParser11() {}

//===--- Test code completions of types.

func resyncParserA1() {}

var topLevelVarType1 : #^TOP_LEVEL_VAR_TYPE_1?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#
// TOP_LEVEL_VAR_TYPE_1-DAG: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}
// TOP_LEVEL_VAR_TYPE_NEGATIVE_1-NOT: Decl[GlobalVar
// TOP_LEVEL_VAR_TYPE_NEGATIVE_1-NOT: Decl[FreeFunc
func resyncParserA1_1() {}

var topLevelVarType2 : [#^TOP_LEVEL_VAR_TYPE_2?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#]

func resyncParserA1_2() {}

var topLevelVarType3 : [#^TOP_LEVEL_VAR_TYPE_3?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#: Int]

func resyncParserA1_3() {}

var topLevelVarType4 : [Int: #^TOP_LEVEL_VAR_TYPE_4?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#]

func resyncParserA1_4() {}

if let topLevelVarType5 : [#^TOP_LEVEL_VAR_TYPE_5?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#] {}

func resyncParserA1_5() {}

guard let topLevelVarType6 : [#^TOP_LEVEL_VAR_TYPE_6?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#] else {}

func resyncParserA1_6() {}

_ = ("a" as #^TOP_LEVEL_EXPR_TYPE_1?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#)
func resyncParserA1_7() {}
_ = ("a" as! #^TOP_LEVEL_EXPR_TYPE_2?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#)
func resyncParserA1_8() {}
_ = ("a" as? #^TOP_LEVEL_EXPR_TYPE_3?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#)
func resyncParserA1_9() {}
_ = ("a" is #^TOP_LEVEL_EXPR_TYPE_4?check=TOP_LEVEL_VAR_TYPE_1;check=TOP_LEVEL_VAR_TYPE_NEGATIVE_1;check=NEGATIVE^#)

func resyncParserA2() {}

//===--- Test code completion in statements.

func resyncParserB1() {}

if (true) {
  #^TOP_LEVEL_STMT_1?check=PLAIN_TOP_LEVEL^#
}

func resyncParserB2() {}

while (true) {
  #^TOP_LEVEL_STMT_2?check=PLAIN_TOP_LEVEL^#
}

func resyncParserB3() {}

repeat {
  #^TOP_LEVEL_STMT_3?check=PLAIN_TOP_LEVEL^#
} while true

func resyncParserB4() {}

for ; ; {
  #^TOP_LEVEL_STMT_4?check=PLAIN_TOP_LEVEL^#
}

func resyncParserB5() {}

for var i = 0; ; {
  #^TOP_LEVEL_STMT_5?check=PLAIN_TOP_LEVEL;check=TOP_LEVEL_STMT_5^#
// TOP_LEVEL_STMT_5: Decl[LocalVar]/Local: i[#<<error type>>#]{{; name=.+$}}
}

func resyncParserB6() {}

for i in [] {
  #^TOP_LEVEL_STMT_6?check=PLAIN_TOP_LEVEL;check=TOP_LEVEL_STMT_6^#
// TOP_LEVEL_STMT_6: Decl[LocalVar]/Local: i[#Any#]{{; name=.+$}}
}

func resyncParserB7() {}

for i in [1, 2, 3] {
  #^TOP_LEVEL_STMT_7?check=PLAIN_TOP_LEVEL;check=TOP_LEVEL_STMT_7^#
// TOP_LEVEL_STMT_7: Decl[LocalVar]/Local: i[#Int#]{{; name=.+$}}
}

func resyncParserB8() {}

for i in unknown_var {
  #^TOP_LEVEL_STMT_8?check=PLAIN_TOP_LEVEL;check=TOP_LEVEL_STMT_8^#
// TOP_LEVEL_STMT_8: Decl[LocalVar]/Local: i[#<<error type>>#]{{; name=.+$}}
}

func resyncParserB9() {}

switch (0, 42) {
  case (0, 0):
    #^TOP_LEVEL_STMT_9?check=PLAIN_TOP_LEVEL^#
}

func resyncParserB10() {}

// rdar://20738314
if true {
    var z = #^TOP_LEVEL_STMT_10?check=PLAIN_TOP_LEVEL^#
} else {
    assertionFailure("Shouldn't be here")
}

func resyncParserB11() {}

// rdar://21346928
func optStr() -> String? { return nil }
let x = (optStr() ?? "autoclosure").#^TOP_LEVEL_AUTOCLOSURE_1?check=AUTOCLOSURE_STRING^#
// AUTOCLOSURE_STRING: Decl[InstanceVar]/CurrNominal/IsSystem: unicodeScalars[#String.UnicodeScalarView#]
// AUTOCLOSURE_STRING: Decl[InstanceVar]/CurrNominal/IsSystem: utf16[#String.UTF16View#]

func resyncParserB12() {}

// rdar://21661308
switch 1 {
  case #^TOP_LEVEL_SWITCH_CASE_1^#
}

// TOP_LEVEL_SWITCH_CASE_1-DAG: Literal[Integer]/None/TypeRelation[Convertible]: 0[#Int#]; name=0
// TOP_LEVEL_SWITCH_CASE_1-DAG: Literal[Boolean]/None:              true[#Bool#]; name=true

func resyncParserB13() {}

#^TOP_LEVEL_BEFORE_GUARD_NAME_1?check=TOP_LEVEL_BEFORE_GUARD_NAME^#
// TOP_LEVEL_BEFORE_GUARD_NAME-NOT: name=guardedName

guard let guardedName = 1 as Int? {
  #^TOP_LEVEL_BEFORE_GUARD_NAME_2?check=TOP_LEVEL_BEFORE_GUARD_NAME^#
}

#^TOP_LEVEL_GUARD_1?check=TOP_LEVEL_GUARD^#

func interstitial() {}

#^TOP_LEVEL_GUARD_2?check=TOP_LEVEL_GUARD^#
// TOP_LEVEL_GUARD: Decl[LocalVar]/Local: guardedName[#Int#]; name=guardedName

func resyncParserB14() {}


"\(#^STRING_INTERP_1?check=STRING_INTERP^#)"
"\(1) \(#^STRING_INTERP_2?check=STRING_INTERP^#) \(2)"
var stringInterp = "\(#^STRING_INTERP_3?check=STRING_INTERP^#)"
_ = "" + "\(#^STRING_INTERP_4?check=STRING_INTERP^#)" + ""
// STRING_INTERP-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]/IsSystem: ['(']{#(value): any Any.Type#}[')'][#Void#];
// STRING_INTERP-DAG: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]; name=FooStruct
// STRING_INTERP-DAG: Decl[FreeFunction]/CurrModule/TypeRelation[Invalid]: fooFunc1()[#Void#];
// STRING_INTERP-DAG: Decl[FreeFunction]/CurrModule: optStr()[#String?#];
// STRING_INTERP-DAG: Decl[GlobalVar]/Local: fooObject[#FooStruct#];
func resyncParserC1() {}

// FOR_COLLECTION-NOT: forIndex
for forIndex in [#^FOR_COLLECTION_1?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#] {}
for forIndex in [1,#^FOR_COLLECTION_2?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#] {}
for forIndex in [1:#^FOR_COLLECTION_3?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#] {}
for forIndex in [#^FOR_COLLECTION_4?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#:] {}
for forIndex in [#^FOR_COLLECTION_5?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#:2] {}
for forIndex in [1:2, #^FOR_COLLECTION_6?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#] {}
for forIndex in [1:2, #^FOR_COLLECTION_7?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#:] {}
for forIndex in [1:2, #^FOR_COLLECTION_8?check=PLAIN_TOP_LEVEL;check=FOR_COLLECTION^#:2] {}


//
//===--- DON'T ADD ANY TESTS AFTER THIS LINE.
//
// These declarations should not show up in top-level code completion results
// because forward references are not allowed at the top level.

struct StructAtEOF {}
// NEGATIVE-NOT: StructAtEOF

extension FooStruct {
  func instanceFuncAtEOF() {}
// NEGATIVE-NOT: instanceFuncAtEOF
}

var varAtEOF : Int
// NEGATIVE-NOT: varAtEOF
