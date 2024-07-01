protocol BaseProtocol {}

extension BaseProtocol {
  /// Say hello
  func hello() { }
}

protocol InheritedProtocol: BaseProtocol {}

extension InheritedProtocol {
  func hello() { }
}

func testInheritBetweenExtensions(x: MyStruct) {
  struct MyStruct: InheritedProtocol {}

  // RUN: %sourcekitd-test -req=cursor -req-opts=retrieve_symbol_graph=1 -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix BETWEEN_EXTENSIONS
  MyStruct().hello()
  // BETWEEN_EXTENSIONS: DOC COMMENT
  // BETWEEN_EXTENSIONS-NEXT: Say hello
  // BETWEEN_EXTENSIONS-NEXT: DOC COMMENT XML
  // BETWEEN_EXTENSIONS: SYMBOL GRAPH BEGIN
  // BETWEEN_EXTENSIONS: Say hello
  // BETWEEN_EXTENSIONS: SYMBOL GRAPH END
}

func testInheritFromExtensionToStruct(x: MyStruct) {
  struct MyStruct: BaseProtocol {
    func hello() {}
  }

  // RUN: %sourcekitd-test -req=cursor -req-opts=retrieve_symbol_graph=1 -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix EXTENSION_TO_STRUCT
  MyStruct().hello()
  // EXTENSION_TO_STRUCT: DOC COMMENT
  // EXTENSION_TO_STRUCT-NEXT: Say hello
  // EXTENSION_TO_STRUCT-NEXT: DOC COMMENT XML
  // EXTENSION_TO_STRUCT: SYMBOL GRAPH BEGIN
  // EXTENSION_TO_STRUCT: Say hello
  // EXTENSION_TO_STRUCT: SYMBOL GRAPH END
}

protocol ProtocolWithDefaultedRequirement {
  /// Doc for the requirement
  func hello()
}

extension ProtocolWithDefaultedRequirement {
  /// Doc for the default implementation
  func hello() {}
}

func testProtocolWithDefaultedRequirement(foo: any ProtocolWithDefaultedRequirement) {
  // RUN: %sourcekitd-test -req=cursor -req-opts=retrieve_symbol_graph=1 -pos=%(line + 1):7 %s -- %s | %FileCheck %s --check-prefix DEFAULTED_REQUIREMENT
  foo.hello()
  // DEFAULTED_REQUIREMENT: DOC COMMENT
  // DEFAULTED_REQUIREMENT-NEXT: Doc for the requirement
  // DEFAULTED_REQUIREMENT-NEXT: DOC COMMENT XML
  // DEFAULTED_REQUIREMENT: SYMBOL GRAPH BEGIN
  // DEFAULTED_REQUIREMENT: Doc for the requirement
  // DEFAULTED_REQUIREMENT: SYMBOL GRAPH END
}


func testDontInheritFromFunctionWithDifferentType() {
  struct MyStruct: InheritedProtocol {
    func hello() -> String { return "Hello" }
  }
  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):30 %s -- %s | %FileCheck %s --check-prefix FROM_FUNCTION_WITH_DIFFERENT_TYPE
  let x: String = MyStruct().hello()
  // FROM_FUNCTION_WITH_DIFFERENT_TYPE: DOC COMMENT
  // FROM_FUNCTION_WITH_DIFFERENT_TYPE-NEXT: DOC COMMENT XML
}

protocol ProtocolWithThrowingFunction {
  /// Say hello
  func hello() throws
}

func testInheritEvenIfThrowingNonThrowingMismatches() {
  struct MyStruct: ProtocolWithThrowingFunction {
    func hello() { }
  }
  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix SATISFY_THROWING_WITH_NON_THROWING
  MyStruct().hello()
  // SATISFY_THROWING_WITH_NON_THROWING: DOC COMMENT
  // SATISFY_THROWING_WITH_NON_THROWING-NEXT: Say hello
  // SATISFY_THROWING_WITH_NON_THROWING-NEXT: DOC COMMENT XML
}

protocol ProtocolWithAccessor {
  /// The greeting
  var greeting: String { get }
}

func testSatisfyAccessorRequirement() {
  struct MyStruct: ProtocolWithAccessor {
    var greeting: String = "hello"
  }
  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix ACCESSOR_REQUIREMENT
  MyStruct().greeting
  // ACCESSOR_REQUIREMENT: DOC COMMENT
  // ACCESSOR_REQUIREMENT-NEXT: The greeting
  // ACCESSOR_REQUIREMENT-NEXT: DOC COMMENT XML
}

protocol ProtocolWithMethodReturningOptional {}
extension ProtocolWithMethodReturningOptional {
  /// hello
  func hello() -> String? { nil }
}

func testInheritFromLessSpecificOverridden() {
  struct MyStruct: ProtocolWithMethodReturningOptional {
    func hello() -> String { "hello" }
  }

  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix INHERIT_FROM_LESS_SPECIFIC_OVERRIDDEN
  MyStruct().hello()
  // INHERIT_FROM_LESS_SPECIFIC_OVERRIDDEN: DOC COMMENT
  // INHERIT_FROM_LESS_SPECIFIC_OVERRIDDEN-NEXT: hello
  // INHERIT_FROM_LESS_SPECIFIC_OVERRIDDEN-NEXT: DOC COMMENT XML
}


protocol ProtocolWithStaticMethod {}
extension ProtocolWithStaticMethod {
  /// hello
  static func hello() {}
}

func testDontInheritFromStaticToNonStatic() {
  struct MyStruct: ProtocolWithStaticMethod {
    func hello() {}
  }

  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):14 %s -- %s | %FileCheck %s --check-prefix DONT_INHERIT_FROM_STATIC_TO_NON_STATIC
  MyStruct().hello()
  // DONT_INHERIT_FROM_STATIC_TO_NON_STATIC: DOC COMMENT
  // DONT_INHERIT_FROM_STATIC_TO_NON_STATIC-NEXT: DOC COMMENT XML
}

protocol ProtocolWithStaticMethodReturningSelf {
  /// hello
  static func hello(_ greeting: String) -> Self {}
}

func testInheritStaticFuncToEnumCase() {
  enum MyEnum: ProtocolWithStaticMethodReturningSelf {
    case hello(String)
  }

  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):10 %s -- %s | %FileCheck %s --check-prefix INHERIT_FROM_STATIC_FUNC_TO_ENUM_CASE
  MyEnum.hello
  // INHERIT_FROM_STATIC_FUNC_TO_ENUM_CASE: DOC COMMENT
  // INHERIT_FROM_STATIC_FUNC_TO_ENUM_CASE-NEXT: hello
  // INHERIT_FROM_STATIC_FUNC_TO_ENUM_CASE-NEXT: DOC COMMENT XML
}
