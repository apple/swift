// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)

// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/../Macros/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-frontend -emit-sil -emit-sorted-sil -Xllvm -sil-full-demangle -profile-generate -profile-coverage-mapping -module-name coverage_macros %s -load-plugin-library %t/%target-library-name(MacroDefinition) | %FileCheck %s --implicit-check-not sil_coverage_map
// RUN: %target-swift-frontend -emit-ir -profile-generate -profile-coverage-mapping -load-plugin-library %t/%target-library-name(MacroDefinition) %s

@attached(accessor)
macro accessViaStorage() = #externalMacro(module: "MacroDefinition", type: "AccessViaStorageMacro")

@attached(
  member,
  names: named(init), named(Storage), named(storage), named(getStorage()), named(method)
)
macro addMembers() = #externalMacro(module: "MacroDefinition", type: "AddMembers")

@freestanding(expression)
macro nestedDeclInExpr() -> () -> Void = #externalMacro(module: "MacroDefinition", type: "NestedDeclInExprMacro")

@freestanding(expression)
macro fnCall<T>(_: () throws -> T) -> T = #externalMacro(module: "MacroDefinition", type: "NullaryFunctionCallMacro")

@freestanding(expression)
macro fnCall<T>(_: () throws -> T, _: () throws -> T) -> (T, T) = #externalMacro(module: "MacroDefinition", type: "NullaryFunctionCallMacro")

@freestanding(expression)
macro id<T>(_: T) -> T = #externalMacro(module: "MacroDefinition", type: "TupleMacro")

@attached(body)
macro BodyMacroWithControlFlow() = #externalMacro(module: "MacroDefinition", type: "BodyMacroWithControlFlow")

@freestanding(declaration, names: arbitrary)
macro declMacroWithControlFlow() = #externalMacro(module: "MacroDefinition", type: "DeclMacroWithControlFlow")

// This needs to be matched up here due to the sorting of the SIL; just make
// sure the counter '2' is for the initialization.
// CHECK-LABEL: sil hidden [lazy_getter] [noinline] @$s15coverage_macros2S1V2z3SiSgvg : $@convention(method) (@inout S1) -> Optional<Int>
// CHECK:       switch_enum {{%[0-9]+}} : $Optional<Optional<Int>>, case #Optional.some!enumelt: {{bb[0-9]}}, case #Optional.none!enumelt: [[INITBB:bb[0-9]]]
// CHECK:       [[INITBB]]
// CHECK-NEXT:  increment_profiler_counter 2

// This needs to be matched up here due to the sorting of the SIL; just make
// sure we emit the counter increments for the error branches.
// CHECK-LABEL: sil hidden @$s15coverage_macros5test2Si_SityKF
// CHECK:       increment_profiler_counter 0,{{.*}}s15coverage_macros5test2Si_SityKF
// CHECK:       {{bb[0-9]+}}({{%[0-9]+}} : $any Error):
// CHECK-NEXT:    increment_profiler_counter 1,{{.*}}s15coverage_macros5test2Si_SityKF
// CHECK:       {{bb[0-9]+}}({{%[0-9]+}} : $any Error):
// CHECK-NEXT:    increment_profiler_counter 2,{{.*}}s15coverage_macros5test2Si_SityKF

// Note we use implicit-check-not, so this test ensures we don't emit
// coverage maps for the macro expansions.

// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros10throwingFnSiyKF
func throwingFn() throws -> Int { 0 }

struct S1 {
  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.x
  var x: Int = 0

  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.y
  var y: Int = 0

  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.z
  var z: Int = #id(.random() ? 3 : 4) // CHECK-NEXT: [[@LINE]]:16 -> [[@LINE]]:38 : 0

  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.z1
  var z1: Int? = try? #id(throwingFn()) // CHECK-NEXT: [[@LINE]]:18 -> [[@LINE]]:40 : 0

  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S1.z2
  var z2: Int? = #id(try? throwingFn()) // CHECK-NEXT: [[@LINE]]:18 -> [[@LINE]]:40 : 0

  // The counter for the initialization region is '2', see the match of s15coverage_macros2S1V2z3SiSgvg above.
  // CHECK-LABEL: sil_coverage_map{{.*}}coverage_macros.S1.z3.getter
  lazy var z3: Int? = #id(try? throwingFn()) // CHECK-NEXT: [[@LINE]]:23 -> [[@LINE]]:45 : 2
}

@addMembers
struct S2 {
  // CHECK-LABEL: sil_coverage_map{{.*}}variable initialization expression of coverage_macros.S2.(_storage
  private var _storage = S1()

  @accessViaStorage
  var x: Int

  // No coverage map for the initializer, as it gets subsumed.
  @accessViaStorage
  var y: Int = 17
}

// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros3fooyyF
func foo() {
  _ = #nestedDeclInExpr()
}

// For cases where control flow happens in the macro expansion, we drop
// all the regions that occur within the expansion, but account for any
// control flow changes when exiting the expansion.
//
// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test1yyKF
func test1() throws {         // CHECK-NEXT: [[@LINE]]:21 -> [[@LINE+2]]:2 : 0
  _ = try #fnCall(throwingFn) // CHECK-NEXT: [[@LINE]]:30 -> [[@LINE+1]]:2 : (0 - 1)
}                             // CHECK-NEXT: }

// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test2Si_SityKF
func test2() throws -> (Int, Int) {            // CHECK-NEXT: [[@LINE]]:35 -> [[@LINE+3]]:2  : 0
  let x = try #fnCall(throwingFn, throwingFn)  // CHECK-NEXT: [[@LINE]]:46 -> [[@LINE+1]]:11 : ((0 - 1) - 2)
  return x                                     // CHECK-NEXT: }
}

// In this case the control flow is entirely contained within the macro, with
// the same exit count, so no change.
//
// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test3SiyF
func test3() -> Int {            // CHECK-NEXT: [[@LINE]]:21 -> [[@LINE+3]]:2 : 0
  let x = #id(.random() ? 3 : 4) // CHECK-NEXT: }
  return x
}

// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test4Si_SityKF
func test4() throws -> (Int, Int) {                // CHECK-NEXT: [[@LINE]]:35 -> [[@LINE+3]]:2  : 0
  let x = try #id(#fnCall(throwingFn, throwingFn)) // CHECK-NEXT: [[@LINE]]:51 -> [[@LINE+1]]:11 : ((0 - 1) - 2)
  return x                                         // CHECK-NEXT: }
}

// 0: entry, 1: first else, 2: first error branch, 3: second else,
// 4: second error branch, 5: third else, 6: third error branch,
// 7: fourth error branch
// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test5Si_S2ityKF
func test5() throws -> (Int, Int, Int) {        // CHECK-NEXT: [[@LINE]]:40 -> [[@LINE+6]]:2  : 0
  let x = #id(.random() ? try throwingFn() : 4) // CHECK-NEXT: [[@LINE]]:48 -> [[@LINE+4]]:19 : (0 - 2)
  let y = #id(.random() ? 5 : try throwingFn()) // CHECK-NEXT: [[@LINE]]:48 -> [[@LINE+3]]:19 : ((0 - 2) - 4)
  let z = #id(.random() ? try throwingFn()
                        : try throwingFn())     // CHECK-NEXT: [[@LINE]]:44 -> [[@LINE+1]]:19 : ((((0 - 2) - 4) - 6) - 7)
  return (x, y, z)                              // CHECK-NEXT: }
}

// Not profiled.
@BodyMacroWithControlFlow
func test6() throws


// Not profiled.
@BodyMacroWithControlFlow
func test7() throws {
  guard .random() else { return }
  print("hello")
}

// CHECK-LABEL: sil_coverage_map{{.*}}s15coverage_macros5test8yyKF
func test8() throws {             // CHECK-NEXT: [[@LINE]]:21   -> [[@LINE+4]]:2 : 0
  guard .random() else { return } // CHECK-NEXT: [[@LINE]]:24   -> [[@LINE]]:34  : 1
                                  // CHECK-NEXT: [[@LINE-1]]:34 -> [[@LINE+2]]:2 : (0 - 1)
  #declMacroWithControlFlow       // CHECK-NEXT: [[@LINE]]:28   -> [[@LINE+1]]:2 : ((0 - 1) - 3)
}
