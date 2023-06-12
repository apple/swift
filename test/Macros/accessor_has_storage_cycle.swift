// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -swift-version 5 -swift-version 5

struct Predicate<T> { }

@freestanding(expression)
macro Predicate<T>(_ body: (T) -> Void) -> Predicate<T> = #externalMacro(module: "A", type: "B")
// expected-warning@-1{{could not be found}}

@attached(accessor)
@attached(peer)
macro Foo<T>(filter: Predicate<T>) = #externalMacro(module: "A", type: "B")
// expected-warning@-1{{could not be found}}
// expected-note@-2 2{{declared here}}

struct Content {
  @Foo(filter: #Predicate<Bool> { $0 == true }) var foo: Bool = true
  //expected-error@-1 2{{could not be found for macro}}
  // expected-warning@-2{{result of operator '==' is unused}}
}
