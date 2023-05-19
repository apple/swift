// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

//===--- Helper types that are used in this test

struct FooStruct {
}

var fooObject: FooStruct

func fooFunc() -> FooStruct {
  return fooObject
}

enum FooEnum {
}

class FooClass {
}

protocol FooProtocol {
  var fooInstanceVar: Int
  typealias FooTypeAlias1
  func fooInstanceFunc0() -> Double
  func fooInstanceFunc1(a: Int) -> Double
  subscript(i: Int) -> Double
}

protocol BarProtocol {
  var barInstanceVar: Int
  typealias BarTypeAlias1
  func barInstanceFunc0() -> Double
  func barInstanceFunc1(a: Int) -> Double
}

typealias FooTypealias = Int

// Global completions
// WITH_GLOBAL_TYPES-DAG: Decl[Struct]/CurrModule:    FooStruct[#FooStruct#]{{; name=.+$}}
// WITH_GLOBAL_TYPES-DAG: Decl[Enum]/CurrModule:      FooEnum[#FooEnum#]{{; name=.+$}}
// WITH_GLOBAL_TYPES-DAG: Decl[Class]/CurrModule:     FooClass[#FooClass#]{{; name=.+$}}
// WITH_GLOBAL_TYPES-DAG: Decl[Protocol]/CurrModule:  FooProtocol[#FooProtocol#]{{; name=.+$}}
// WITH_GLOBAL_TYPES-DAG: Decl[TypeAlias]/CurrModule: FooTypealias[#Int#]{{; name=.+$}}

// Global completions at expression position
// WITH_GLOBAL_TYPES_EXPR-DAG: Decl[Struct]/CurrModule:    FooStruct[#FooStruct#]{{; name=.+$}}
// WITH_GLOBAL_TYPES_EXPR-DAG: Decl[Enum]/CurrModule:      FooEnum[#FooEnum#]{{; name=.+$}}
// WITH_GLOBAL_TYPES_EXPR-DAG: Decl[Class]/CurrModule:     FooClass[#FooClass#]{{; name=.+$}}
// WITH_GLOBAL_TYPES_EXPR-DAG: Decl[Protocol]/CurrModule/Flair[RareType]: FooProtocol[#FooProtocol#]{{; name=.+$}}
// WITH_GLOBAL_TYPES_EXPR-DAG: Decl[TypeAlias]/CurrModule: FooTypealias[#Int#]{{; name=.+$}}

// GLOBAL_NEGATIVE-NOT: Decl.*: fooObject
// GLOBAL_NEGATIVE-NOT: Decl.*: fooFunc

// WITHOUT_GLOBAL_TYPES-NOT: Decl.*: FooStruct
// WITHOUT_GLOBAL_TYPES-NOT: Decl.*: FooEnum
// WITHOUT_GLOBAL_TYPES-NOT: Decl.*: FooClass
// WITHOUT_GLOBAL_TYPES-NOT: Decl.*: FooProtocol
// WITHOUT_GLOBAL_TYPES-NOT: Decl.*: FooTypealias

// ERROR_COMMON: found code completion token

//===---
//===--- Test that we include 'Self' type while completing inside a protocol.
//===---

// TYPE_IN_PROTOCOL-DAG: Decl[GenericTypeParam]/Local: Self[#Self#]{{; name=.+$}}

protocol TestSelf1 {
  func instanceFunc() -> #^TYPE_IN_PROTOCOL_1?check=TYPE_IN_PROTOCOL;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we include types from generic parameter lists.
//===---
// FIXME: tests for constructors and destructors.

func testTypeInParamGeneric1<
    GenericFoo : FooProtocol,
    GenericBar : FooProtocol & BarProtocol,
    GenericBaz>(a: #^TYPE_IN_FUNC_PARAM_GENERIC_1?check=TYPE_IN_FUNC_PARAM_GENERIC_1;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

// Generic parameters of the function.
// TYPE_IN_FUNC_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

struct TestTypeInParamGeneric2<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  func testTypeInParamGeneric2(a: #^TYPE_IN_FUNC_PARAM_GENERIC_2?check=TYPE_IN_FUNC_PARAM_GENERIC_2;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// TYPE_IN_FUNC_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}

struct TestTypeInParamGeneric3 {
  func testTypeInParamGeneric3<
      GenericFoo : FooProtocol,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(a: #^TYPE_IN_FUNC_PARAM_GENERIC_3?check=TYPE_IN_FUNC_PARAM_GENERIC_3;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// TYPE_IN_FUNC_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

struct TestTypeInParamGeneric4<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  func testTypeInParamGeneric4<
      GenericFoo : FooProtocol,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(a: #^TYPE_IN_FUNC_PARAM_GENERIC_4?check=TYPE_IN_FUNC_PARAM_GENERIC_4;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// Generic parameters of the struct.
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}
// Generic parameters of the function.
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_4-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

struct TestTypeInParamGeneric5<StructGenericFoo> {
  struct TestTypeInParamGeneric5a<StructGenericBar> {
    struct TestTypeInParamGeneric5b<StructGenericBaz> {
      func testTypeInParamGeneric5<GenericFoo>(a: #^TYPE_IN_FUNC_PARAM_GENERIC_5?check=TYPE_IN_FUNC_PARAM_GENERIC_5;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
    }
  }
}

// Generic parameters of the containing structs.
// TYPE_IN_FUNC_PARAM_GENERIC_5-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_5-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// TYPE_IN_FUNC_PARAM_GENERIC_5-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}
// Generic parameters of the function.
// TYPE_IN_FUNC_PARAM_GENERIC_5-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}

struct TestTypeInConstructorParamGeneric1<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  init(a: #^TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_1?check=TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_1;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_1-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}

struct TestTypeInConstructorParamGeneric2 {
  init<GenericFoo : FooProtocol,
       GenericBar : FooProtocol & BarProtocol,
       GenericBaz>(a: #^TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_2?check=TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_2;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_2-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

struct TestTypeInConstructorParamGeneric3<
    StructGenericFoo : FooProtocol,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
  init<GenericFoo : FooProtocol,
       GenericBar : FooProtocol & BarProtocol,
       GenericBaz>(a: #^TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3?check=TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

// Generic parameters of the struct.
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: StructGenericFoo[#StructGenericFoo#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: StructGenericBar[#StructGenericBar#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: StructGenericBaz[#StructGenericBaz#]{{; name=.+$}}
// Generic parameters of the constructor.
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericFoo[#GenericFoo#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericBar[#GenericBar#]{{; name=.+$}}
// TYPE_IN_CONSTRUCTOR_PARAM_GENERIC_3-DAG: Decl[GenericTypeParam]/Local: GenericBaz[#GenericBaz#]{{; name=.+$}}

// No tests for destructors: destructors don't have parameters.

//===---
//===--- Test that we don't duplicate generic parameters.
//===---

struct GenericStruct<T> {
	func foo() -> #^TYPE_IN_RETURN_GEN_PARAM_NO_DUP^#
}
class A<T> {
	var foo: #^TYPE_IVAR_GEN_PARAM_NO_DUP^#

	subscript(_ arg: Int) -> #^TYPE_IN_SUBSCR_GEN_PARAM_NO_DUP^#
}

// TYPE_IN_RETURN_GEN_PARAM_NO_DUP-DAG: Decl[GenericTypeParam]/Local: T[#T#]; name=T
// TYPE_IN_RETURN_GEN_PARAM_NO_DUP-NOT: Decl[GenericTypeParam]/Local: T[#T#]; name=T

// TYPE_IVAR_GEN_PARAM_NO_DUP-DAG: Decl[GenericTypeParam]/Local: T[#T#]; name=T
// TYPE_IVAR_GEN_PARAM_NO_DUP-NOT: Decl[GenericTypeParam]/Local: T[#T#]; name=T

// TYPE_IN_SUBSCR_GEN_PARAM_NO_DUP-DAG: Decl[GenericTypeParam]/Local: T[#T#]; name=T
// TYPE_IN_SUBSCR_GEN_PARAM_NO_DUP-NOT: Decl[GenericTypeParam]/Local: T[#T#]; name=T

//===---
//===--- Test that we can complete types in variable declarations.
//===---

func testTypeInLocalVarInFreeFunc1() {
  var localVar: #^TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInLocalVarInFreeFunc2() {
  struct NestedStruct {}
  class NestedClass {}
  enum NestedEnum {
    case NestedEnumX(Int)
  }

  typealias NestedTypealias = Int

  var localVar: #^TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2?check=TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}
// TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2-DAG: Decl[Struct]/Local:    NestedStruct[#NestedStruct#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2-DAG: Decl[Class]/Local:     NestedClass[#NestedClass#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2-DAG: Decl[Enum]/Local:      NestedEnum[#NestedEnum#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_FREE_FUNC_2-DAG: Decl[TypeAlias]/Local: NestedTypealias[#Int#]{{; name=.+$}}

class TestTypeInLocalVarInMemberFunc1 {
  struct NestedStruct {}
  class NestedClass {}
  enum NestedEnum {
    case NestedEnumX(Int)
  }

  typealias NestedTypealias = Int

  init() {
    var localVar: #^TYPE_IN_LOCAL_VAR_IN_CONSTRUCTOR_1?check=TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  deinit {
    var localVar: #^TYPE_IN_LOCAL_VAR_IN_DESTRUCTOR_1?check=TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  func test() {
    var localVar: #^TYPE_IN_LOCAL_VAR_IN_INSTANCE_FUNC_1?check=TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }
}
// TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1-DAG: Decl[Struct]/CurrNominal:    NestedStruct[#NestedStruct#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1-DAG: Decl[Class]/CurrNominal:     NestedClass[#NestedClass#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1-DAG: Decl[Enum]/CurrNominal:      NestedEnum[#NestedEnum#]{{; name=.+$}}
// TYPE_IN_LOCAL_VAR_IN_MEMBER_FUNC_1-DAG: Decl[TypeAlias]/CurrNominal: NestedTypealias[#Int#]{{; name=.+$}}

var TypeInGlobalVar1: #^TYPE_IN_GLOBAL_VAR_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

//===---
//===--- Test that we can complete types in typealias declarations.
//===---

typealias TypeInTypealias1 = #^TYPE_IN_TYPEALIAS_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

typealias TypeInTypealias2 = (#^TYPE_IN_TYPEALIAS_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func resyncParser0() {}

typealias TypeInTypealias3 = ((#^TYPE_IN_TYPEALIAS_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func resyncParser1() {}

//===---
//===--- Test that we can complete types in associated type declarations.
//===---

protocol AssocType1 {
  associatedtype AssocType = #^TYPE_IN_ASSOC_TYPE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we can complete types in inheritance clause of associated type declarations.
//===---

protocol AssocType1 {
  associatedtype AssocType : #^TYPE_IN_ASSOC_TYPE_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we can complete types in extension declarations.
//===---

extension #^TYPE_IN_EXTENSION_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

//===---
//===--- Test that we can complete types in the extension inheritance clause.
//===---

extension TypeInExtensionInheritance1 : #^TYPE_IN_EXTENSION_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

extension TypeInExtensionInheritance2 : #^TYPE_IN_EXTENSION_INHERITANCE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

extension TypeInExtensionInheritance3 : FooProtocol, #^TYPE_IN_EXTENSION_INHERITANCE_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

//===---
//===--- Test that we can complete types in the struct inheritance clause.
//===---

struct TypeInStructInheritance1 : #^TYPE_IN_STRUCT_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

struct TypeInStructInheritance2 : , #^TYPE_IN_STRUCT_INHERITANCE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

struct TypeInStructInheritance3 : FooProtocol, #^TYPE_IN_STRUCT_INHERITANCE_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

// FIXME: 'check' shold be 'WITH_GLOBAL_TYPES'
struct TypeInStructInheritance4 : FooProtocol., #^TYPE_IN_STRUCT_INHERITANCE_4?check=WITH_GLOBAL_TYPES_EXPR^#

struct TypeInStructInheritance5 : #^TYPE_IN_STRUCT_INHERITANCE_5?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

struct TypeInStructInheritance6 : , #^TYPE_IN_STRUCT_INHERITANCE_6?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

struct TypeInStructInheritance7 : FooProtocol, #^TYPE_IN_STRUCT_INHERITANCE_7?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

// FIXME: 'check' shold be 'WITH_GLOBAL_TYPES'
struct TypeInStructInheritance8 : FooProtocol., #^TYPE_IN_STRUCT_INHERITANCE_8?check=WITH_GLOBAL_TYPES_EXPR^# {
}

//===---
//===--- Test that we can complete types in the class inheritance clause.
//===---

class TypeInClassInheritance1 : #^TYPE_IN_CLASS_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

class TypeInClassInheritance2 : #^TYPE_IN_CLASS_INHERITANCE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

//===---
//===--- Test that we can complete types in the enum inheritance clause.
//===---

enum TypeInEnumInheritance1 : #^TYPE_IN_ENUM_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

enum TypeInEnumInheritance2 : #^TYPE_IN_ENUM_INHERITANCE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

//===---
//===--- Test that we can complete types in the protocol inheritance clause.
//===---

protocol TypeInProtocolInheritance1 : #^TYPE_IN_PROTOCOL_INHERITANCE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

protocol TypeInProtocolInheritance2 : #^TYPE_IN_PROTOCOL_INHERITANCE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# {
}

//===---
//===--- Test that we can complete types in tuple types.
//===---

func testTypeInTupleType1() {
  var localVar: (#^TYPE_IN_TUPLE_TYPE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType2() {
  var localVar: (a: #^TYPE_IN_TUPLE_TYPE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType3() {
  var localVar: (Int, #^TYPE_IN_TUPLE_TYPE_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType4() {
  var localVar: (a: Int, #^TYPE_IN_TUPLE_TYPE_4?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType5() {
  var localVar: (Int, a: #^TYPE_IN_TUPLE_TYPE_5?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType6() {
  var localVar: (a:, #^TYPE_IN_TUPLE_TYPE_6?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInTupleType7() {
  var localVar: (a: b: #^TYPE_IN_TUPLE_TYPE_7?xfail=FIXME^#
}

//===---
//===--- Test that we can complete types in function types.
//===---

func testTypeInFunctionType1() {
  var localVar: #^TYPE_IN_FUNCTION_TYPE_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^# ->
}

func testTypeInFunctionType2() {
  var localVar: (#^TYPE_IN_FUNCTION_TYPE_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#) -> ()
}

func testTypeInFunctionType3() {
  var localVar: () -> #^TYPE_IN_FUNCTION_TYPE_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInFunctionType4() {
  var localVar: (Int) -> #^TYPE_IN_FUNCTION_TYPE_4?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInFunctionType5() {
  var localVar: (a: Int) -> #^TYPE_IN_FUNCTION_TYPE_5?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInFunctionType6() {
  var localVar: (a: Int, ) -> #^TYPE_IN_FUNCTION_TYPE_6?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we can complete types in protocol compositions.
//===---

func testTypeInProtocolComposition1() {
  var localVar: protocol<#^TYPE_IN_PROTOCOL_COMPOSITION_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInProtocolComposition2() {
  var localVar: protocol<, #^TYPE_IN_PROTOCOL_COMPOSITION_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

func testTypeInProtocolComposition3() {
  var localVar: protocol<FooProtocol, #^TYPE_IN_PROTOCOL_COMPOSITION_3?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we can complete types from extensions and base classes.
//===---

class VarBase1 {
  var instanceVarBase1: #^TYPE_IN_INSTANCE_VAR_1?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func paramNestedTypesBase1(a: #^TYPE_IN_FUNC_PARAM_NESTED_TYPES_1?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func localVarBaseTest1() {
    var localVar: #^TYPE_IN_LOCAL_VAR_1?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  // Define types after all tests to test delayed parsing of decls.

  struct BaseNestedStruct {}
  class BaseNestedClass {}
  enum BaseNestedEnum {
    case BaseEnumX(Int)
  }

  typealias BaseNestedTypealias = Int
}

extension VarBase1 {
  var instanceVarBaseExt1: #^TYPE_IN_INSTANCE_VAR_2?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func paramNestedTypesBaseExt1(a: #^TYPE_IN_FUNC_PARAM_NESTED_TYPES_2?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func localVarBaseExtTest1() {
    var localVar: #^TYPE_IN_LOCAL_VAR_2?check=VAR_BASE_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  // Define types after all tests to test delayed parsing of decls.

  struct BaseExtNestedStruct {}
  class BaseExtNestedClass {}
  enum BaseExtNestedEnum {
    case BaseExtEnumX(Int)
  }

  typealias BaseExtNestedTypealias = Int
}

// From VarBase1
// VAR_BASE_1_TYPES-DAG: Decl[Struct]/CurrNominal:    BaseNestedStruct[#VarBase1.BaseNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[Class]/CurrNominal:     BaseNestedClass[#VarBase1.BaseNestedClass#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[Enum]/CurrNominal:      BaseNestedEnum[#VarBase1.BaseNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[TypeAlias]/CurrNominal: BaseNestedTypealias[#Int#]{{; name=.+$}}
// From VarBase1 extension
// VAR_BASE_1_TYPES-DAG: Decl[Struct]/CurrNominal:    BaseExtNestedStruct[#VarBase1.BaseExtNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[Class]/CurrNominal:     BaseExtNestedClass[#VarBase1.BaseExtNestedClass#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[Enum]/CurrNominal:      BaseExtNestedEnum[#VarBase1.BaseExtNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_TYPES-DAG: Decl[TypeAlias]/CurrNominal: BaseExtNestedTypealias[#Int#]{{; name=.+$}}

// From VarBase1
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Struct]/CurrNominal:    BaseNestedStruct[#BaseNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Class]/CurrNominal:     BaseNestedClass[#BaseNestedClass#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Enum]/CurrNominal:      BaseNestedEnum[#BaseNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/CurrNominal: BaseNestedTypealias[#Int#]{{; name=.+$}}
// From VarBase1 extension
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Struct]/CurrNominal:    BaseExtNestedStruct[#BaseExtNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Class]/CurrNominal:     BaseExtNestedClass[#BaseExtNestedClass#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[Enum]/CurrNominal:      BaseExtNestedEnum[#BaseExtNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/CurrNominal: BaseExtNestedTypealias[#Int#]{{; name=.+$}}

// From VarBase1
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Struct]/CurrNominal:    .BaseNestedStruct[#VarBase1.BaseNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Class]/CurrNominal:     .BaseNestedClass[#VarBase1.BaseNestedClass#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Enum]/CurrNominal:      .BaseNestedEnum[#VarBase1.BaseNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[TypeAlias]/CurrNominal: .BaseNestedTypealias[#Int#]{{; name=.+$}}
// From VarBase1 extension
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Struct]/CurrNominal:    .BaseExtNestedStruct[#VarBase1.BaseExtNestedStruct#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Class]/CurrNominal:     .BaseExtNestedClass[#VarBase1.BaseExtNestedClass#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[Enum]/CurrNominal:      .BaseExtNestedEnum[#VarBase1.BaseExtNestedEnum#]{{; name=.+$}}
// VAR_BASE_1_NO_DOT_TYPES-DAG: Decl[TypeAlias]/CurrNominal: .BaseExtNestedTypealias[#Int#]{{; name=.+$}}

class VarDerived1 : VarBase1 {
  var instanceVarDerived1 : #^TYPE_IN_INSTANCE_VAR_3?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func paramNestedTypesDerived1(a: #^TYPE_IN_FUNC_PARAM_NESTED_TYPES_3?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func localVarDerivedTest1() {
    var localVar : #^TYPE_IN_LOCAL_VAR_3?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  // Define types after all tests to test delayed parsing of decls.

  struct DerivedNestedStruct {}
  class DerivedNestedClass {}
  enum DerivedNestedEnum {
    case DerivedEnumX(Int)
  }

  typealias DerivedNestedTypealias = Int
}

extension VarDerived1 {
  var instanceVarDerivedExt1 : #^TYPE_IN_INSTANCE_VAR_4?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func paramNestedTypesDerivedExt1(a: #^TYPE_IN_FUNC_PARAM_NESTED_TYPES_4?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

  func localVarDerivedExtTest1() {
    var localVar : #^TYPE_IN_LOCAL_VAR_4?check=VAR_DERIVED_1_TYPES_INCONTEXT;check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
  }

  // Define types after all tests to test delayed parsing of decls.

  struct DerivedExtNestedStruct {}
  class DerivedExtNestedClass {}
  enum DerivedExtNestedEnum {
    case DerivedExtEnumX(Int)
  }

  typealias DerivedExtNestedTypealias = Int
}

// From VarBase1
// VAR_DERIVED_1_TYPES-DAG: Decl[Struct]/Super:          BaseNestedStruct[#VarBase1.BaseNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Class]/Super:           BaseNestedClass[#VarBase1.BaseNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Enum]/Super:            BaseNestedEnum[#VarBase1.BaseNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[TypeAlias]/Super:       BaseNestedTypealias[#Int#]{{; name=.+$}}
// From VarBase1 extension
// VAR_DERIVED_1_TYPES-DAG: Decl[Struct]/Super:          BaseExtNestedStruct[#VarBase1.BaseExtNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Class]/Super:           BaseExtNestedClass[#VarBase1.BaseExtNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Enum]/Super:            BaseExtNestedEnum[#VarBase1.BaseExtNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[TypeAlias]/Super:       BaseExtNestedTypealias[#Int#]{{; name=.+$}}
// From VarDerived1
// VAR_DERIVED_1_TYPES-DAG: Decl[Struct]/CurrNominal:    DerivedNestedStruct[#VarDerived1.DerivedNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Class]/CurrNominal:     DerivedNestedClass[#VarDerived1.DerivedNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Enum]/CurrNominal:      DerivedNestedEnum[#VarDerived1.DerivedNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[TypeAlias]/CurrNominal: DerivedNestedTypealias[#Int#]{{; name=.+$}}
// From VarDerived1 extension
// VAR_DERIVED_1_TYPES-DAG: Decl[Struct]/CurrNominal:    DerivedExtNestedStruct[#VarDerived1.DerivedExtNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Class]/CurrNominal:     DerivedExtNestedClass[#VarDerived1.DerivedExtNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[Enum]/CurrNominal:      DerivedExtNestedEnum[#VarDerived1.DerivedExtNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES-DAG: Decl[TypeAlias]/CurrNominal: DerivedExtNestedTypealias[#Int#]{{; name=.+$}}

// From VarBase1
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Struct]/Super:          BaseNestedStruct[#VarBase1.BaseNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Class]/Super:           BaseNestedClass[#VarBase1.BaseNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Enum]/Super:            BaseNestedEnum[#VarBase1.BaseNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/Super:       BaseNestedTypealias[#Int#]{{; name=.+$}}
// From VarBase1 extension
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Struct]/Super:          BaseExtNestedStruct[#VarBase1.BaseExtNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Class]/Super:           BaseExtNestedClass[#VarBase1.BaseExtNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Enum]/Super:            BaseExtNestedEnum[#VarBase1.BaseExtNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/Super:       BaseExtNestedTypealias[#Int#]{{; name=.+$}}
// From VarDerived1
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Struct]/CurrNominal:    DerivedNestedStruct[#DerivedNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Class]/CurrNominal:     DerivedNestedClass[#DerivedNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Enum]/CurrNominal:      DerivedNestedEnum[#DerivedNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/CurrNominal: DerivedNestedTypealias[#Int#]{{; name=.+$}}
// From VarDerived1 extension
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Struct]/CurrNominal:    DerivedExtNestedStruct[#DerivedExtNestedStruct#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Class]/CurrNominal:     DerivedExtNestedClass[#DerivedExtNestedClass#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[Enum]/CurrNominal:      DerivedExtNestedEnum[#DerivedExtNestedEnum#]{{; name=.+$}}
// VAR_DERIVED_1_TYPES_INCONTEXT-DAG: Decl[TypeAlias]/CurrNominal: DerivedExtNestedTypealias[#Int#]{{; name=.+$}}

//===---
//===--- Test that we can complete based on user-provided type-identifier.
//===---

func testTypeIdentifierBase1(a: VarBase1.#^TYPE_IDENTIFIER_BASE_1?check=VAR_BASE_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
func testTypeIdentifierBase2(a: Int, b: VarBase1.#^TYPE_IDENTIFIER_BASE_2?check=VAR_BASE_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
func testTypeIdentifierBase3(a: unknown_type, b: VarBase1.#^TYPE_IDENTIFIER_BASE_3?check=VAR_BASE_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
func testTypeIdentifierBase4(a: , b: VarBase1.#^TYPE_IDENTIFIER_BASE_4?check=VAR_BASE_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func testTypeIdentifierBaseNoDot1(a: VarBase1#^TYPE_IDENTIFIER_BASE_NO_DOT_1?check=VAR_BASE_1_NO_DOT_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func testTypeIdentifierBaseNoDot2() {
  var localVar : protocol<VarBase1#^TYPE_IDENTIFIER_BASE_NO_DOT_2?check=VAR_BASE_1_NO_DOT_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

typealias testTypeIdentifierBaseNoDot3 = VarBase1#^TYPE_IDENTIFIER_BASE_NO_DOT_3?check=VAR_BASE_1_NO_DOT_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func testTypeIdentifierDerived1(a: VarDerived1.#^TYPE_IDENTIFIER_DERIVED_1?check=VAR_DERIVED_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func testTypeIdentifierDerived2() {
  var localVar : protocol<VarDerived1.#^TYPE_IDENTIFIER_DERIVED_2?check=VAR_DERIVED_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

typealias testTypeIdentifierDerived3 = VarDerived1.#^TYPE_IDENTIFIER_DERIVED_3?check=VAR_DERIVED_1_TYPES;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

func testTypeIdentifierGeneric1<
    GenericFoo : FooProtocol
    >(a: GenericFoo.#^TYPE_IDENTIFIER_GENERIC_1?check=TYPE_IDENTIFIER_GENERIC_1;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

// TYPE_IDENTIFIER_GENERIC_1: Begin completions, 2 items
// TYPE_IDENTIFIER_GENERIC_1-DAG: Decl[AssociatedType]/CurrNominal: FooTypeAlias1{{; name=.+$}}
// TYPE_IDENTIFIER_GENERIC_1-DAG: Keyword/None:          Type[#GenericFoo.Type#]

func testTypeIdentifierGeneric2<
    GenericFoo : FooProtocol & BarProtocol
    >(a: GenericFoo.#^TYPE_IDENTIFIER_GENERIC_2?check=TYPE_IDENTIFIER_GENERIC_2;check=WITHOUT_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#

// TYPE_IDENTIFIER_GENERIC_2: Begin completions, 3 items
// TYPE_IDENTIFIER_GENERIC_2-DAG: Decl[AssociatedType]/CurrNominal: BarTypeAlias1{{; name=.+$}}
// TYPE_IDENTIFIER_GENERIC_2-DAG: Decl[AssociatedType]/CurrNominal: FooTypeAlias1{{; name=.+$}}
// TYPE_IDENTIFIER_GENERIC_2-DAG: Keyword/None:          Type[#GenericFoo.Type#]

func testTypeIdentifierGeneric3<
    GenericFoo>(a: GenericFoo.#^TYPE_IDENTIFIER_GENERIC_3^#

// TYPE_IDENTIFIER_GENERIC_3-DAG: Keyword/None:          Type[#GenericFoo.Type#]
// TYPE_IDENTIFIER_GENERIC_3-NOT: Keyword/CurrNominal:    self[#GenericFoo#]

func testTypeIdentifierIrrelevant1() {
  var a: Int
  #^TYPE_IDENTIFIER_IRRELEVANT_1^#
}
// TYPE_IDENTIFIER_IRRELEVANT_1-DAG: Decl[LocalVar]/Local: a[#Int#]{{; name=.+$}}
// TYPE_IDENTIFIER_IRRELEVANT_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{; name=.+$}}

//===---
//===--- Test that we can complete types in 'as' cast.
//===---

func testAsCast1(a: Int) {
  a as #^INSIDE_AS_CAST_1?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#
}

//===---
//===--- Test that we can complete generic typealiases.
//===---

func testGenericTypealias1() {
  typealias MyPair<T> = (T, T)
  let x: #^GENERIC_TYPEALIAS_1^#
}
// FIXME: should we use the alias name in the annotation?
// GENERIC_TYPEALIAS_1: Decl[TypeAlias]/Local: MyPair[#(T, T)#];
func testGenericTypealias2() {
  typealias MyPair<T> = (T, T)
  let x: MyPair<#^GENERIC_TYPEALIAS_2?check=WITH_GLOBAL_TYPES;check=GLOBAL_NEGATIVE^#>
}

// In generic argument
struct GenStruct<T> { }
let a : GenStruct<#^GENERIC_ARGS_TOPLEVEL_VAR?check=WITH_GLOBAL_TYPES^#
func foo1(x: GenStruct<#^GENERIC_ARGS_TOPLEVEL_PARAM?check=WITH_GLOBAL_TYPES^#
func foo2() -> GenStruct<#^GENERIC_ARGS_TOPLEVEL_RETURN?check=WITH_GLOBAL_TYPES^#
class _TestForGenericArg_ {
  let a : GenStruct<#^GENERIC_ARGS_MEMBER_VAR?check=WITH_GLOBAL_TYPES^#
  func foo1(x: GenStruct<#^GENERIC_ARGS_MEMBER_PARAM?check=WITH_GLOBAL_TYPES^#
  func foo2() -> GenStruct<#^GENERIC_ARGS_MEMBER_RETURN?check=WITH_GLOBAL_TYPES^#
}
func _testForGenericArg_() {
  let a : GenStruct<#^GENERIC_ARGS_LOCAL_VAR?check=WITH_GLOBAL_TYPES^#
  func foo1(x: GenStruct<#^GENERIC_ARGS_LOCAL_PARAM?check=WITH_GLOBAL_TYPES^#
  func foo2() -> GenStruct<#^GENERIC_ARGS_LOCAL_RETURN?check=WITH_GLOBAL_TYPES^#
}

func testProtocol() {
  let _: FooProtocol.#^PROTOCOL_DOT_1^#
// PROTOCOL_DOT_1: Begin completions, 3 items
// PROTOCOL_DOT_1-DAG: Decl[AssociatedType]/CurrNominal:   FooTypeAlias1; name=FooTypeAlias1
// PROTOCOL_DOT_1-DAG: Keyword/None:                       Protocol[#(any FooProtocol).Type#]; name=Protocol
// PROTOCOL_DOT_1-DAG: Keyword/None:                       Type[#any FooProtocol.Type#]; name=Type
}

//===---
//===--- Test we can complete unbound generic types
//===---

public final class Task<Success> {
  public enum Inner {
    public typealias Failure = Int
    case success(Success)
    case failure(Failure)
  }
}
extension Task.Inner {
  public init(left error: Failure) {
    fatalError()
  }
}
extension Task.Inner.#^UNBOUND_DOT_1?check=UNBOUND_DOT^# {}
func testUnbound(x: Task.Inner.#^UNBOUND_DOT_2?check=UNBOUND_DOT^#) {}
// UNBOUND_DOT-DAG: Decl[TypeAlias]/CurrNominal:        Failure[#Int#]; name=Failure
// UNBOUND_DOT-DAG: Keyword/None:                       Type[#Task.Inner.Type#]; name=Type


protocol MyProtocol {}
struct OuterStruct<U>  {
  class Inner<V>: MyProtocol {}
}

func testUnbound2(x: OuterStruct<Int>.Inner.#^UNBOUND_DOT_3^#) {}
// UNBOUND_DOT_3-DAG: Keyword/None:                       Type[#OuterStruct<Int>.Inner.Type#]; name=Type

// rdar://problem/67102794
struct HasProtoAlias {
  typealias ProtoAlias = FooProtocol
}
extension FooStruct: HasProtoAlias.#^EXTENSION_INHERITANCE_1?check=EXTENSION_INHERITANCE^# {}

struct ContainExtension {
  extension FooStruct: HasProtoAlias.#^EXTENSION_INHERITANCE_2?check=EXTENSION_INHERITANCE^# {}
}
// EXTENSION_INHERITANCE: Begin completions, 2 items
// EXTENSION_INHERITANCE-DAG: Decl[TypeAlias]/CurrNominal:        ProtoAlias[#FooProtocol#];
// EXTENSION_INHERITANCE-DAG: Keyword/None:                       Type[#HasProtoAlias.Type#];

var _: (() -> #^IN_POSTFIX_BASE_1?check=WITH_GLOBAL_TYPES^#)?
var _: (() -> #^IN_POSTFIX_BASE_2?check=WITH_GLOBAL_TYPES^#)!
var _: (() -> #^IN_POSTFIX_BASE_3?check=WITH_GLOBAL_TYPES^#)[1]
var _: (() -> #^IN_POSTFIX_BASE_4?check=WITH_GLOBAL_TYPES^#).Protocol
var _: (() -> #^IN_POSTFIX_BASE_5?check=WITH_GLOBAL_TYPES^#).Type

struct HaveNested {
    struct Nested {}
}

var _: HaveNested.#^IN_POSTFIX_BASE_MEMBER_1?check=POSTFIX_BASE_MEMBER^#?
var _: HaveNested.#^IN_POSTFIX_BASE_MEMBER_2?check=POSTFIX_BASE_MEMBER^#!
var _: HaveNested.#^IN_POSTFIX_BASE_MEMBER_3?check=POSTFIX_BASE_MEMBER^#[1]
var _: HaveNested.#^IN_POSTFIX_BASE_MEMBER_4?check=POSTFIX_BASE_MEMBER^#.Protocol
var _: HaveNested.#^IN_POSTFIX_BASE_MEMBER_5?check=POSTFIX_BASE_MEMBER^#.Type

// POSTFIX_BASE_MEMBER: Begin completions, 2 items
// POSTFIX_BASE_MEMBER-DAG: Decl[Struct]/CurrNominal:           Nested[#HaveNested.Nested#];
// POSTFIX_BASE_MEMBER-DAG: Keyword/None:                       Type[#HaveNested.Type#];
