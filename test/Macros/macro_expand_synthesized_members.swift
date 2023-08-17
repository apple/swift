// REQUIRES: swift_swift_parser, executable_test
//
// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5 -verify-ignore-unknown
// RUN: %target-build-swift -g -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@attached(
  member,
  names: named(init), named(Storage), named(storage), named(getStorage()), named(method), named(init(other:))
)
macro addMembers() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

@attached(
  member,
  names: named(`init`), named(Storage), named(storage), named(getStorage()), named(method)
)
macro addMembersQuotedInit() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

#if TEST_DIAGNOSTICS
@addMembers
import Swift
// expected-error@-2 {{'member' macro cannot be attached to import}}
#endif

@addMembers
struct S {
  func useSynthesized() {
    S.method()
    print(type(of: getStorage()))
  }
}

@attached(
  member,
  names: named(extInstanceMethod), named(extStaticMethod)
)
macro addExtMembers() = #externalMacro(module: "MacroDefinition", type: "AddExtMembers")

@addExtMembers
extension S { }

let s = S()

// CHECK: synthesized method
// CHECK: Storage
s.useSynthesized()

// Members added via extension.
s.extInstanceMethod()
S.extStaticMethod()

@attached(member, names: arbitrary)
macro addArbitraryMembers() = #externalMacro(module: "MacroDefinition", type: "AddArbitraryMembers")

@addArbitraryMembers
struct MyType {}

// CHECK: MyType1
// CHECK: MyType2
// CHECK: MyType3
print(MyType.MyType1.self)
print(MyType.MyType2.self)
print(MyType.MyType3.self)

@attached(
  member,
  names: named(RawValue), named(rawValue), named(init)
)
public macro NewType<T>() = #externalMacro(module: "MacroDefinition", type: "NewTypeMacro")

@NewType<String>
public struct MyString {}

// CHECK: String
// CHECK: hello
let myString = MyString("hello")
print(MyString.RawValue.self)
print(myString.rawValue)

struct Base {
  static func member() -> Base { .init() }
}

@attached(member) macro empty(
  _ : Base
) = #externalMacro(module: "MacroDefinition", type: "EmptyMacro")

@empty(.member())
struct TestMacroTypechecking {}

// Macros adding to an enum
@attached(member, names: named(unknown), arbitrary)
public macro ExtendableEnum() = #externalMacro(module: "MacroDefinition", type: "ExtendableEnum")

@ExtendableEnum
enum ElementType {
  case paper
}

print(ElementType.paper.unknown())

#if TEST_DIAGNOSTICS
@addMembersQuotedInit
struct S2 {
// expected-note@-2 {{in expansion of macro 'addMembersQuotedInit' on struct 'S2' here}}
  func useSynthesized() {
    S.method()
    print(type(of: getStorage()))
  }
}
#endif
