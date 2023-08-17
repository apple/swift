// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath -swift-version 5

// Diagnostics testing
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS

// Diagnostics testing by importing macros from a module
// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/freestanding_macro_library.swiftmodule %S/Inputs/freestanding_macro_library.swift -module-name freestanding_macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/freestanding_macro_library_2.swiftmodule %S/Inputs/freestanding_macro_library_2.swift -module-name freestanding_macro_library_2 -load-plugin-library %t/%target-library-name(MacroDefinition) -I %t

// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -I %t -DIMPORT_MACRO_LIBRARY

// RUN: not %target-swift-frontend -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -serialize-diagnostics-path %t/macro_expand.dia %s -emit-macro-expansion-files no-diagnostics > %t/macro-printing.txt
// RUN: c-index-test -read-diagnostics %t/macro_expand.dia 2>&1 | %FileCheck -check-prefix CHECK-DIAGS %s

// RUN: %FileCheck %s  --check-prefix CHECK-MACRO-PRINTED < %t/macro-printing.txt

// Debug info SIL testing
// RUN: %target-swift-frontend -swift-version 5 -emit-sil -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name MacroUser -o - -g | %FileCheck --check-prefix CHECK-SIL %s

// Debug info IR testing
// RUN: %target-swift-frontend -swift-version 5 -emit-ir -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name MacroUser -o - -g | %FileCheck --check-prefix CHECK-IR %s

// Execution testing
// RUN: %target-build-swift -swift-version 5 -g -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -Xfrontend -emit-dependencies-path -Xfrontend %t/main.d -Xfrontend -emit-reference-dependencies-path -Xfrontend %t/main.swiftdeps
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// Plugin search path and loaded module trace testing
// RUN: %target-swift-frontend -swift-version 5 -emit-sil -plugin-path %t %s -module-name MacroUser -emit-loaded-module-trace -o %t/loaded_module_trace
// RUN: %FileCheck -check-prefix=CHECK-MODULE-TRACE %s < %t/loaded_module_trace.trace.json

// CHECK-MODULE-TRACE: {{libMacroDefinition.dylib|libMacroDefinition.so|MacroDefinition.dll}}

#if IMPORT_MACRO_LIBRARY
import freestanding_macro_library
import freestanding_macro_library_2
#else
@freestanding(declaration, names: named(StructWithUnqualifiedLookup))
macro structWithUnqualifiedLookup() = #externalMacro(module: "MacroDefinition", type: "DefineStructWithUnqualifiedLookupMacro")

@freestanding(declaration)
macro anonymousTypes(_: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")

@freestanding(declaration)
macro freestandingWithClosure<T>(_ value: T, body: (T) -> T) = #externalMacro(module: "MacroDefinition", type: "EmptyDeclarationMacro")

@freestanding(declaration, names: arbitrary) macro bitwidthNumberedStructs(_ baseName: String) = #externalMacro(module: "MacroDefinition", type: "DefineBitwidthNumberedStructsMacro")

@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

@freestanding(declaration, names: arbitrary) macro bitwidthNumberedStructs(_ baseName: String, blah: Bool) = #externalMacro(module: "MacroDefinition", type: "DefineBitwidthNumberedStructsMacro")

@freestanding(declaration, names: named(value)) macro varValue() = #externalMacro(module: "MacroDefinition", type: "VarValueMacro")

#endif

#if TEST_DIAGNOSTICS
@freestanding(declaration)
macro NotCovered() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

struct MemberNotCovered {
  #NotCovered
  // expected-note@-1 {{in expansion of macro 'NotCovered' here}}

  // CHECK-DIAGS: error: declaration name 'value' is not covered by macro 'NotCovered'
  // CHECK-DIAGS: CONTENTS OF FILE @__swiftmacro_9MacroUser16MemberNotCoveredV33_4361AD9339943F52AE6186DD51E04E91Ll0dE0fMf0_.swift
  // CHECK-DIAGS: var value: Int
  // CHECK-DIAGS: END CONTENTS OF FILE
}

@attached(peer)
macro Invalid() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

@Invalid
struct Bad {}
// expected-note@-2 18 {{in expansion of macro 'Invalid' on struct 'Bad' here}}

// CHECK-DIAGS: error: macro expansion cannot introduce import
// CHECK-DIAGS: error: macro expansion cannot introduce precedence group
// CHECK-DIAGS: error: macro expansion cannot introduce macro
// CHECK-DIAGS: error: macro expansion cannot introduce extension
// CHECK-DIAGS: error: macro expansion cannot introduce '@main' type
// CHECK-DIAGS: error: declaration name 'MyMain' is not covered by macro 'Invalid'
// CHECK-DIAGS: error: declaration name 'Array' is not covered by macro 'Invalid'
// CHECK-DIAGS: error: declaration name 'Dictionary' is not covered by macro 'Invalid'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'BooleanLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'ExtendedGraphemeClusterType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'FloatLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'IntegerLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'StringLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type 'UnicodeScalarType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type '_ColorLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type '_ImageLiteralType'
// CHECK-DIAGS: error: macro expansion cannot introduce default literal type '_FileReferenceLiteralType'

// CHECK-DIAGS: CONTENTS OF FILE @__swiftmacro_9MacroUser3Bad7InvalidfMp_.swift
// CHECK-DIAGS: import Swift
// CHECK-DIAGS: precedencegroup MyPrecedence {
// CHECK-DIAGS: }
// CHECK-DIAGS: @attached(member) macro myMacro()
// CHECK-DIAGS: extension Int {
// CHECK-DIAGS: }
// CHECK-DIAGS: @main
// CHECK-DIAGS: struct MyMain {
// CHECK-DIAGS:   static func main() {
// CHECK-DIAGS:   }
// CHECK-DIAGS: }
// CHECK-DIAGS: typealias Array = Void
// CHECK-DIAGS: typealias Dictionary = Void
// CHECK-DIAGS: typealias BooleanLiteralType = Void
// CHECK-DIAGS: typealias ExtendedGraphemeClusterType = Void
// CHECK-DIAGS: typealias FloatLiteralType = Void
// CHECK-DIAGS: typealias IntegerLiteralType = Void
// CHECK-DIAGS: typealias StringLiteralType = Void
// CHECK-DIAGS: typealias UnicodeScalarType = Void
// CHECK-DIAGS: typealias _ColorLiteralType = Void
// CHECK-DIAGS: typealias _ImageLiteralType = Void
// CHECK-DIAGS: typealias _FileReferenceLiteralType = Void
// CHECK-DIAGS: END CONTENTS OF FILE
#endif

@freestanding(expression) macro customFileID() -> String = #externalMacro(module: "MacroDefinition", type: "FileIDMacro")
@freestanding(expression) macro fileID<T: ExpressibleByStringLiteral>() -> T = #externalMacro(module: "MacroDefinition", type: "FileIDMacro")
@freestanding(expression) macro recurse(_: Bool) = #externalMacro(module: "MacroDefinition", type: "RecursiveMacro")
@freestanding(expression) macro assert(_: Bool) = #externalMacro(module: "MacroDefinition", type: "AssertMacro")

func testFileID(a: Int, b: Int) {
  // CHECK: MacroUser/macro_expand.swift
  print("Result is \(#customFileID)")
  // CHECK-SIL: sil_scope [[SRC_SCOPE:[0-9]+]] { loc "{{.*}}macro_expand.swift":[[@LINE-3]]:6 parent {{.*}}testFileID
  // CHECK-SIL: sil_scope [[EXPANSION_SCOPE:[0-9]+]] { loc "{{.*}}macro_expand.swift":[[@LINE-2]]:22 parent [[SRC_SCOPE]]
  // CHECK-SIL: sil_scope [[MACRO_SCOPE:[0-9]+]] { loc "@__swiftmacro{{.*}}":1:1 parent @$s9MacroUser10testFileID1a1bySi_SitF06customdE0fMf_ {{.*}} inlined_at [[EXPANSION_SCOPE]] }
  // CHECK-SIL: string_literal utf8 "MacroUser/macro_expand.swift", loc "@__swiftmacro_9MacroUser10testFileID1a1bySi_SitF06customdE0fMf_.swift":1:1, scope [[MACRO_SCOPE]]
  // CHECK-IR-DAG: !DISubprogram(name: "customFileID", linkageName: "$s9MacroUser10testFileID1a1bySi_SitF06customdE0fMf_"

  // CHECK: Builtin result is MacroUser/macro_expand.swift
  // CHECK-AST: macro_expansion_expr type='String'{{.*}}name=line
  print("Builtin result is \(#fileID)")
  print(
    /// CHECK-IR-DAG: ![[L1:[0-9]+]] = distinct !DILocation(line: [[@LINE+1]], column: 5
    #addBlocker(
      /// CHECK-IR-DAG: ![[L2:[0-9]+]] = distinct !DILocation({{.*}}inlinedAt: ![[L1]])
      /// CHECK-IR-DAG: ![[L3:[0-9]+]] = !DILocation({{.*}}inlinedAt: ![[L2]])
      #stringify(a - b)
      )
    )
}

testFileID(a: 1, b: 2)

@freestanding(expression) macro stringifyAndTry<T>(_ value: T) -> (T, String) =
    #externalMacro(module: "MacroDefinition", type: "StringifyAndTryMacro")

func testStringify(a: Int, b: Int) {
  let s = #stringify(a + b)
  print(s)

  // CHECK-AST: macro_expansion_expr type='(Int, String)'{{.*}}name=stringify
  // CHECK-AST-NEXT: argument_list
  // CHECK-AST: tuple_expr type='(Int, String)' location=Macro expansion of #stringify

  let (b, s2) = #stringify({ () -> Bool in return true })
  // CHECK-AST: macro_expansion_expr type='(() -> Bool, String)'{{.*}}name=stringify
  // CHECK-AST-NEXT: argument_list
  // CHECK-AST: tuple_expr type='(() -> Bool, String)' location=Macro expansion of #stringify

  let (b2, s3) = #stringify<Double>(1 + 2)
  // CHECK-AST: macro_expansion_expr type='(Double, String)'{{.*}}name=stringify
  // CHECK-AST-NEXT: argument_list
  // CHECK-AST: tuple_expr type='(Double, String)' location=Macro expansion of #stringify

  _ = (b, b2, s2, s3)
}

func testAssert(a: Int, b: Int) {
  #assert(a == b)
}

public struct Outer {
  var value: Int = 0
  public func test() {
    let (a, b) = #stringify(1 + value)

    let (c, d) = #stringify({ x in
      x + 1
    })

    _ = a
    _ = b
    _ = c
    _ = d
  }
}

// CHECK: (2, "a + b")
testStringify(a: 1, b: 1)

protocol P { }
extension Int: P { }

// Stringify with closures that have local types.
@available(SwiftStdlib 5.1, *)
func testStringifyWithLocalTypes() {
  _ = #stringify({
    struct LocalType: P {
      static var name: String = "Taylor"
      var something: some P { self }
    }

    func f() -> some P { return LocalType().something }
  })
}

func maybeThrowing() throws -> Int { 5 }

#if TEST_DIAGNOSTICS
@freestanding(expression) @discardableResult
macro discardableStringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

func testDiscardableStringify(x: Int) {
  #stringify(x + 1) // expected-warning{{expression of type '(Int, String)' is unused}}
  #discardableStringify(x + 1)
}
#endif

#if TEST_DIAGNOSTICS
// This causes an error when non-'Bool' value is passed.
@freestanding(expression) macro assertAny<T>(_ value: T) = #externalMacro(module: "MacroDefinition", type: "AssertMacro")

func testNested() {
  struct Nested { }
  _ = #stringify(#assertAny(Nested()))
  // expected-note@-1 {{in expansion of macro 'stringify' here}}
// CHECK-DIAGS-NOT: error: cannot convert value of type 'Nested' to expected argument type 'Bool'
// CHECK-DIAGS: @__swiftmacro_9MacroUser10testNestedyyF9stringifyfMf_9assertAnyfMf_.swift:1:8: error: cannot convert value of type 'Nested' to expected argument type 'Bool'
// CHECK-DIAGS-NOT: error: cannot convert value of type 'Nested' to expected argument type 'Bool'
}
#endif

func testStringifyWithThrows() throws {
  // Okay, we can put the try inside or outside
  _ = try #stringify(maybeThrowing())
  _ = #stringify(try maybeThrowing())

#if TEST_DIAGNOSTICS
  // FIXME: Lots of duplicate notes here
  _ = #stringify(maybeThrowing()) // expected-note 4{{in expansion of macro 'stringify' here}}

    // CHECK-DIAGS: @__swiftmacro_9MacroUser23testStringifyWithThrowsyyKF9stringifyfMf1_.swift:1:2: error: call can throw but is not marked with 'try'
#endif

  // The macro adds the 'try' for us.
  _ = #stringifyAndTry(maybeThrowing())
}

func testStringifyWithLocalType() throws {
  _ =  #stringify({
    struct QuailError: Error {}
    throw QuailError()
    })
}

@freestanding(expression) macro addBlocker<T>(_ value: T) -> T = #externalMacro(module: "MacroDefinition", type: "AddBlocker")

struct OnlyAdds {
  static func +(lhs: OnlyAdds, rhs: OnlyAdds) -> OnlyAdds { lhs }
}

func testAddBlocker(a: Int, b: Int, c: Int, oa: OnlyAdds) {
  _ = #addBlocker(a * b * c)
#if TEST_DIAGNOSTICS
  _ = #addBlocker(a + b * c) // expected-error{{blocked an add; did you mean to subtract? (from macro 'addBlocker')}}
  // expected-note@-1{{use '-'}}{{21-22=-}}
  _ = #addBlocker(oa + oa) // expected-error{{blocked an add; did you mean to subtract? (from macro 'addBlocker')}}
  // expected-note@-1{{in expansion of macro 'addBlocker' here}}
  // expected-note@-2{{use '-'}}{{22-23=-}}

  // CHECK-DIAGS: @__swiftmacro_9MacroUser14testAddBlocker1a1b1c2oaySi_S2iAA8OnlyAddsVtF03addE0fMf1_.swift:1:4: error: binary operator '-' cannot be applied to two 'OnlyAdds' operands [] []
  // CHECK-DIAGS: CONTENTS OF FILE @__swiftmacro_9MacroUser14testAddBlocker1a1b1c2oaySi_S2iAA8OnlyAddsVtF03addE0fMf1_.swift:
  // CHECK-DIAGS-NEXT: Original source range: {{.*}}macro_expand.swift:[[@LINE-6]]:7 - {{.*}}macro_expand.swift:[[@LINE-6]]:27
  // CHECK-DIAGS-NEXT: oa - oa
  // CHECK-DIAGS-NEXT: END CONTENTS OF FILE

  _ = #addBlocker({ // expected-note{{in expansion of macro 'addBlocker' here}}

      print("hello")
      print(oa + oa) // expected-error{{blocked an add; did you mean to subtract? (from macro 'addBlocker')}}
      // expected-note@-1{{use '-'}}
      print(oa + oa) // expected-error{{blocked an add; did you mean to subtract? (from macro 'addBlocker')}}
      // expected-note@-1{{use '-'}}
    }())

  // Check recursion.
  #recurse(false) // okay
  #recurse(true) // expected-note{{in expansion of macro 'recurse' here}}
#endif
}

@freestanding(expression) macro leftHandOperandFinder<T>(_ value: T) -> T = #externalMacro(module: "MacroDefinition", type: "LeftHandOperandFinderMacro")


// Test source location information.
func testSourceLocations(x: Int, yolo: Int, zulu: Int) {
  // CHECK-MACRO-PRINTED: Source range for LHS is "MacroUser/macro_expand.swift": [[@LINE+3]]:5-[[@LINE+3]]:13
  // CHECK-MACRO-PRINTED: Source range for LHS is "MacroUser/macro_expand.swift": [[@LINE+2]]:5-[[@LINE+2]]:6
  _ = #leftHandOperandFinder(
    x + yolo + zulu
  )
}

// Make sure we don't crash with declarations produced by expansions.
@freestanding(expression) macro nestedDeclInExpr() -> () -> Void = #externalMacro(module: "MacroDefinition", type: "NestedDeclInExprMacro")

func testNestedDeclInExpr() {
  let _: () -> Void = #nestedDeclInExpr
}

// Test non-arbitrary names
@freestanding(declaration, names: named(A), named(B), named(foo), named(addOne))
macro defineDeclsWithKnownNames() = #externalMacro(module: "MacroDefinition", type: "DefineDeclsWithKnownNamesMacro")

// Freestanding macros are not in inlined scopes.
// CHECK-SIL: sil_scope {{.*}} { loc "@__swiftmacro_9MacroUser016testFreestandingA9ExpansionyyF4Foo2L_V25defineDeclsWithKnownNamesfMf0_.swift"{{.*}} -> Int }

// FIXME: Macros producing arbitrary names are not supported yet
#if false
#bitwidthNumberedStructs("MyIntGlobal")

#bitwidthNumberedStructs("MyIntGlobalTwo", blah: false)

let blah = false
#bitwidthNumberedStructs("MyIntGlobalThree", blah: blah)
#endif

// Test unqualified lookup from within a macro expansion
let world = 3 // to be used by the macro expansion below
#structWithUnqualifiedLookup()
_ = StructWithUnqualifiedLookup().foo()

#anonymousTypes { "hello" }

func testFreestandingMacroExpansion() {
  // Explicit structs to force macros to be parsed as decl.
  struct Foo {
    static let singleton = Foo()

    static let s2 = Foo.singleton

    #bitwidthNumberedStructs("MyIntOne")
  }

  // CHECK: MyIntOne8
  print(Foo.MyIntOne8.self)
  // CHECK: MyIntOne16
  print(Foo.MyIntOne16.self)
  // CHECK: MyIntOne32
  print(Foo.MyIntOne32.self)
  // CHECK: MyIntOne64
  print(Foo.MyIntOne64.self)

  struct Foo2 {
    #bitwidthNumberedStructs("MyIntTwo", blah: false)
    #defineDeclsWithKnownNames()
  }
  // CHECK: MyIntTwo8
  print(Foo2.MyIntTwo8.self)
  // CHECK: MyIntTwo16
  print(Foo2.MyIntTwo16.self)
  // CHECK: MyIntTwo32
  print(Foo2.MyIntTwo32.self)
  // CHECK: MyIntTwo64
  print(Foo2.MyIntTwo64.self)
  // CHECK: A
  print(Foo2.A.self)
  // CHECK: B
  print(Foo2.B.self)
  // CHECK: 1
  print(Foo2().foo)
  // CHECK: 2
  print(Foo2().addOne(1))

  #if TEST_DIAGNOSTICS
  struct Foo3 {
    #bitwidthNumberedStructs("BUG", blah: false)
    // expected-note@-1 4{{in expansion of macro 'bitwidthNumberedStructs' here}}
    // CHECK-DIAGS: CONTENTS OF FILE @__swiftmacro_9MacroUser016testFreestandingA9ExpansionyyF4Foo3L_V23bitwidthNumberedStructsfMf0_.swift
    // CHECK-DIAGS: struct BUG {
    // CHECK-DIAGS:   func $s9MacroUser016testFreestandingA9ExpansionyyF4Foo3L_V23bitwidthNumberedStructsfMf0_6methodfMu_()
    // CHECK-DIAGS:   func $s9MacroUser016testFreestandingA9ExpansionyyF4Foo3L_V23bitwidthNumberedStructsfMf0_6methodfMu0{{_?}}()
  }
  #endif

  // FIXME: Arbitrary name lookup is not yet supported.
  // HECK: MyIntGlobal8
  // print(MyIntGlobal8.self)
  // HECK: MyIntGlobal16
  // print(MyIntGlobal16.self)
  // HECK: MyIntGlobal32
  // print(MyIntGlobal32.self)
  // HECK: MyIntGlobal64
  // print(MyIntGlobal64.self)

  #anonymousTypes { "hello" }
}
testFreestandingMacroExpansion()

// Explicit structs to force macros to be parsed as decl.
var globalBool = true
struct ContainerOfNumberedStructs {
  #bitwidthNumberedStructs("MyIntOne")
  #bitwidthNumberedStructs("MyIntTwo", blah: globalBool)
}

// Avoid re-type-checking declaration macro arguments.
func testFreestandingWithClosure(i: Int) {
  #freestandingWithClosure(i) { x in x }

  #freestandingWithClosure(i) {
    let x = $0
    return x
  }
}

// Nested macros with closures
@freestanding(expression) macro coerceToInt<T>(_ value: T) -> Int = #externalMacro(module: "MacroDefinition", type: "CoerceToIntMacro")

func testFreestandingClosureNesting() {
  _ = #stringify({ () -> Int in
    #coerceToInt(2)
  })
}

// Freestanding declaration macros that produce local variables
func testLocalVarsFromDeclarationMacros() {
  #varValue
}

// Variadic macro
@freestanding(declaration, names: arbitrary) macro emptyDecl(_: String...) = #externalMacro(module: "MacroDefinition", type: "EmptyDeclarationMacro")

struct TakesVariadic {
  #emptyDecl("foo", "bar")
}

// Funkiness with static functions introduced via macro expansions.
@freestanding(declaration, names: named(foo())) public macro staticFooFunc() = #externalMacro(module: "MacroDefinition", type: "StaticFooFuncMacro")
@freestanding(declaration, names: arbitrary) public macro staticFooFuncArbitrary() = #externalMacro(module: "MacroDefinition", type: "StaticFooFuncMacro")

class HasAnExpandedStatic {
  #staticFooFunc()
}

class HasAnExpandedStatic2 {
  #staticFooFuncArbitrary()
}

func testHasAnExpandedStatic() {
#if TEST_DIAGNOSTICS
  foo() // expected-error{{cannot find 'foo' in scope}}
#endif
}

@freestanding(declaration, names: named(==)) public macro addSelfEqualsOperator() = #externalMacro(module: "MacroDefinition", type: "SelfAlwaysEqualOperator")
@freestanding(declaration, names: arbitrary) public macro addSelfEqualsOperatorArbitrary() = #externalMacro(module: "MacroDefinition", type: "SelfAlwaysEqualOperator")
@attached(member, names: named(==)) public macro AddSelfEqualsMemberOperator() = #externalMacro(module: "MacroDefinition", type: "SelfAlwaysEqualOperator")
@attached(member, names: arbitrary) public macro AddSelfEqualsMemberOperatorArbitrary() = #externalMacro(module: "MacroDefinition", type: "SelfAlwaysEqualOperator")

struct HasEqualsSelf {
  #addSelfEqualsOperator
}

struct HasEqualsSelf2 {
  #addSelfEqualsOperatorArbitrary
}

@AddSelfEqualsMemberOperator
struct HasEqualsSelf3 {
}

@AddSelfEqualsMemberOperatorArbitrary
struct HasEqualsSelf4 {
}

protocol SelfEqualsBoolProto { // expected-note 4{{where 'Self' =}}
  static func ==(lhs: Self, rhs: Bool) -> Bool
}

struct HasEqualsSelfP: SelfEqualsBoolProto {
  #addSelfEqualsOperator
}

struct HasEqualsSelf2P: SelfEqualsBoolProto {
  #addSelfEqualsOperatorArbitrary
}

@AddSelfEqualsMemberOperator
struct HasEqualsSelf3P: SelfEqualsBoolProto {
}

@AddSelfEqualsMemberOperatorArbitrary
struct HasEqualsSelf4P: SelfEqualsBoolProto {
}

func testHasEqualsSelf(
  x: HasEqualsSelf, y: HasEqualsSelf2, z: HasEqualsSelf3, w: HasEqualsSelf4,
  xP: HasEqualsSelfP, yP: HasEqualsSelf2P, zP: HasEqualsSelf3P,
  wP: HasEqualsSelf4P
) {
#if TEST_DIAGNOSTICS
  // Global operator lookup doesn't find member operators introduced by macros.
  _ = (x == true) // expected-error{{referencing operator function '=='}}
  _ = (y == true) // expected-error{{referencing operator function '=='}}
  _ = (z == true) // expected-error{{referencing operator function '=='}}
  _ = (w == true) // expected-error{{referencing operator function '=='}}
#endif

  // These should be found through the protocol.
  _ = (xP == true)
  _ = (yP == true)
  _ = (zP == true)
  _ = (wP == true)
}

// Macro whose implementation is both an expression and declaration macro.
@freestanding(declaration)
macro AsDeclMacro<T>(_ value: T) = #externalMacro(module: "MacroDefinition", type: "ExprAndDeclMacro")

@freestanding(expression)
macro AsExprMacro<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "ExprAndDeclMacro")

func testExpressionAndDeclarationMacro() {
  #AsExprMacro(1 + 1) // expected-warning{{expression of type '(Int, String)' is unused}}
  struct Inner {
    #AsDeclMacro(1 + 1)
  }
  #AsDeclMacro(1 + 1)
}

// Expression macro implementation with declaration macro role
@freestanding(declaration) macro stringifyAsDeclMacro<T>(_ value: T) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")
func testExpressionAsDeclarationMacro() {
#if TEST_DIAGNOSTICS
  #stringifyAsDeclMacro(1+1)
  // expected-error@-1{{macro implementation type 'StringifyMacro' doesn't conform to required protocol 'DeclarationMacro' (from macro 'stringifyAsDeclMacro')}}
#endif
}

// Deprecated macro
@available(*, deprecated, message: "This macro is deprecated.")
@freestanding(expression) macro deprecatedStringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

@available(*, deprecated, message: "This macro is deprecated.")
@freestanding(declaration) macro deprecatedStringifyAsDeclMacro<T>(_ value: T) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

func testDeprecated() {
  // expected-warning@+1{{'deprecatedStringify' is deprecated: This macro is deprecated.}}
  _ = #deprecatedStringify(1 + 1)
}

#if TEST_DIAGNOSTICS
struct DeprecatedStructWrapper {
  // expected-error@+2{{macro implementation type 'StringifyMacro' doesn't conform to required protocol 'DeclarationMacro' (from macro 'deprecatedStringifyAsDeclMacro')}}
  // expected-warning@+1{{'deprecatedStringifyAsDeclMacro' is deprecated: This macro is deprecated.}}
  #deprecatedStringifyAsDeclMacro(1 + 1)
}
#endif
