// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// First check for no errors.
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition)

// Check for expected errors.
// RUN: not %target-swift-frontend -typecheck -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -DTEST_DIAGNOSTICS %s > %t/diags.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DIAGS %s < %t/diags.txt

// Check that the expansion buffer are as expected.
// RUN: %target-swift-frontend -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) %s -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt

// Execution testing
// RUN: %target-build-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@attached(accessor)
macro myPropertyWrapper() =
    #externalMacro(module: "MacroDefinition", type: "PropertyWrapperMacro")

@attached(accessor)
macro myPropertyWrapperSkipsComputed() =
    #externalMacro(module: "MacroDefinition", type: "PropertyWrapperSkipsComputedMacro")

struct Date { }

struct MyWrapperThingy<T> {
  var storage: T

  var wrappedValue: T {
    get {
      print("Getting value \(storage)")
      return storage
    }

    set {
      print("Setting value \(newValue)")
      storage = newValue
    }
  }
}

struct MyStruct {
  var _name: MyWrapperThingy<String> = .init(storage: "Hello")
  var _birthDate: MyWrapperThingy<Date?> = .init(storage: nil)
  var _favoriteColor: MyWrapperThingy<String> = .init(storage: "Blue")

  @myPropertyWrapper
  var name: String
  // CHECK-DUMP: @__swiftmacro_15accessor_macros8MyStructV4name17myPropertyWrapperfMa_.swift
  // CHECK-DUMP: get {
  // CHECK-DUMP:   _name.wrappedValue
  // CHECK-DUMP: }
  // CHECK-DUMP: set {
  // CHECK-DUMP:   _name.wrappedValue = newValue
  // CHECK-DUMP: }

  @myPropertyWrapper
  var birthDate: Date?
  // CHECK-DUMP: @__swiftmacro_15accessor_macros8MyStructV9birthDate17myPropertyWrapperfMa_.swift
  // CHECK-DUMP: get {
  // CHECK-DUMP:   _birthDate.wrappedValue
  // CHECK-DUMP: }
  // CHECK-DUMP: set {
  // CHECK-DUMP:   _birthDate.wrappedValue = newValue
  // CHECK-DUMP: }

  @myPropertyWrapperSkipsComputed
  var age: Int? {
    get { nil }
  }

  @myPropertyWrapper
  var favoriteColor: String {
    didSet { fatalError("Boom") }
  }
}

// Test that the fake-property-wrapper-introduced accessors execute properly at
// runtime.
var ms = MyStruct()

// CHECK: Getting value Hello
_ = ms.name

// CHECK-NEXT: Setting value World
ms.name = "World"

// CHECK-NEXT: Setting value Yellow
ms.favoriteColor = "Yellow"

#if TEST_DIAGNOSTICS
struct MyBrokenStruct {
  var _birthDate: MyWrapperThingy<Date?> = .init(storage: nil)

  @myPropertyWrapper
  var birthDate: Date? {
    // CHECK-DIAGS: variable already has a getter
    // CHECK-DIAGS: in expansion of macro
    // CHECK-DIAGS: previous definition of getter here
    get { fatalError("Boom") }

    // CHECK-DIAGS: variable already has a setter
    // CHECK-DIAGS: in expansion of macro
    // CHECK-DIAGS: previous definition of setter here
    set { fatalError("Boom") }
  }
}

@myPropertyWrapper
struct CannotHaveAccessors {}
// CHECK-DIAGS: 'accessor' macro cannot be attached to struct ('CannotHaveAccessors')
#endif
