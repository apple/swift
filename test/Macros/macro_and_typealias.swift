// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/variadic_macros.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-typecheck-verify-swift -disable-availability-checking -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5

@freestanding(expression) public macro Print<each Value>(_ value: repeat each Value) = #externalMacro(module: "MacroDefinition", type: "PrintMacro")
@freestanding(expression) public macro OtherPrint<each Value>(_ value: repeat each Value) = #externalMacro(module: "MacroDefinition", type: "PrintMacro")
@freestanding(expression) public macro ConcretePrint(_ value: Any) = #externalMacro(module: "MacroDefinition", type: "PrintMacro")
@freestanding(expression) public macro MultiPrint(_ value: Any) = #externalMacro(module: "MacroDefinition", type: "PrintMacro")

public struct Printer<Value> {
  init(_: (Value) -> Void) {}
}

public struct MultiPrinter<T, U> {
  // expected-note@-1 {{'T' declared as parameter to type 'MultiPrinter'}}
  // expected-note@-2 {{'U' declared as parameter to type 'MultiPrinter'}}
}

typealias Print = Printer
typealias OtherPrint<T> = Printer<T>
typealias ConcretePrint = Printer<Any>
typealias MultiPrint = MultiPrinter

struct Test {
  struct Object {
    var prop: Int
  }

  func test() {
    let _ = Print<Object> { // Ok
      compute(root: $0, \.prop)
    }

    let _ = Print<Object, Int> {
      // expected-error@-1 {{generic type 'Print' specialized with too many type parameters (got 2, but expected 1)}}
    }

    let _ = OtherPrint<Object> { // Ok
      compute(root: $0, \.prop)
    }

    let _ = ConcretePrint<Object> { // expected-error {{cannot specialize non-generic type 'ConcretePrint' (aka 'Printer<Any>')}}
      compute(root: $0, \.prop) // expected-error {{value of type 'Any' has no member 'prop'}}
      // expected-note@-1 {{cast 'Any' to 'AnyObject' or use 'as!' to force downcast to a more specific type to access members}}
    }

    let _ = MultiPrint<Int>()
    // expected-error@-1 {{generic type 'MultiPrint' specialized with too few type parameters (got 1, but expected 2)}}
    // expected-error@-2 {{generic parameter 'T' could not be inferred}}
    // expected-error@-3 {{generic parameter 'U' could not be inferred}}
  }

  func compute<R, V>(root: R, _: KeyPath<R, V>) {}
}
