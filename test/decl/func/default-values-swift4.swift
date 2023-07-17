// RUN: %target-typecheck-verify-swift -swift-version 4
// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-testing

private func privateFunction() {}
// expected-note@-1 2{{global function 'privateFunction()' is not '@usableFromInline' or public}}
fileprivate func fileprivateFunction() {}
// expected-note@-1 2{{global function 'fileprivateFunction()' is not '@usableFromInline' or public}}
func internalFunction() {}
// expected-note@-1 2{{global function 'internalFunction()' is not '@usableFromInline' or public}}
@usableFromInline func usableFromInlineFunction() {}
public func publicFunction() {}

func internalIntFunction() -> Int {}
// expected-note@-1 {{global function 'internalIntFunction()' is not '@usableFromInline' or public}}

private func privateFunction2() {}
// expected-note@-1 {{global function 'privateFunction2()' is not '@usableFromInline' or public}}
fileprivate func fileprivateFunction2() {}
// expected-note@-1 {{global function 'fileprivateFunction2()' is not '@usableFromInline' or public}}
func internalFunction2() {}
// expected-note@-1 {{global function 'internalFunction2()' is not '@usableFromInline' or public}}

func internalIntFunction2() -> Int {}
// expected-note@-1 {{global function 'internalIntFunction2()' is not '@usableFromInline' or public}}

func internalFunctionWithDefaultValue(
    x: Int = {
      struct Nested {}
      // OK

      publicFunction()
      // OK
      usableFromInlineFunction()
      // OK
      internalFunction()
      // OK
      fileprivateFunction()
      // OK
      privateFunction()
      // OK

      return 0
    }(),
    y: Int = internalIntFunction()) {}

@usableFromInline func usableFromInlineFunctionWithDefaultValue(
    x: Int = {
      struct Nested {}
      // expected-error@-1 {{type 'Nested' cannot be nested inside a default argument value}}

      publicFunction()
      // OK
      usableFromInlineFunction()
      // OK
      internalFunction2()
      // expected-error@-1 {{global function 'internalFunction2()' is internal and cannot be referenced from a default argument value}}
      fileprivateFunction2()
      // expected-error@-1 {{global function 'fileprivateFunction2()' is fileprivate and cannot be referenced from a default argument value}}
      privateFunction2()
      // expected-error@-1 {{global function 'privateFunction2()' is private and cannot be referenced from a default argument value}}

      return 0
    }(),
    y: Int = internalIntFunction2()) {}
    // expected-error@-1 {{global function 'internalIntFunction2()' is internal and cannot be referenced from a default argument value}}

public func publicFunctionWithDefaultValue(
    x: Int = {
      struct Nested {}
      // expected-error@-1 {{type 'Nested' cannot be nested inside a default argument value}}

      // FIXME: Some errors below are diagnosed twice

      publicFunction()

      usableFromInlineFunction()

      internalFunction()
      // expected-error@-1 {{global function 'internalFunction()' is internal and cannot be referenced from a default argument value}}

      fileprivateFunction()
      // expected-error@-1 {{global function 'fileprivateFunction()' is fileprivate and cannot be referenced from a default argument value}}

      privateFunction()
      // expected-error@-1 {{global function 'privateFunction()' is private and cannot be referenced from a default argument value}}

      return 0
    }(),
    y: Int = internalIntFunction()) {}
    // expected-error@-1 {{global function 'internalIntFunction()' is internal and cannot be referenced from a default argument value}}

// https://github.com/apple/swift/issues/48131
public class MyClass {
  public func method<T>(_: T.Type = T.self) -> T { }
}

public func evilCode(
  x: Int = {
    let _ = publicFunction()
    let _ = usableFromInlineFunction()

    func localFunction() {
      publicFunction()
      usableFromInlineFunction()
    }
    return 0
  }()) {}

private func privateIntFunction() -> Int {} // expected-note {{global function 'privateIntFunction()' is not '@usableFromInline' or public}}

public struct HasSubscript {
  public subscript(x: Int = {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside a default argument value}}

    publicFunction()

    usableFromInlineFunction()

    internalFunction()
    // expected-error@-1 {{global function 'internalFunction()' is internal and cannot be referenced from a default argument value}}

    fileprivateFunction()
    // expected-error@-1 {{global function 'fileprivateFunction()' is fileprivate and cannot be referenced from a default argument value}}

    privateFunction()
    // expected-error@-1 {{global function 'privateFunction()' is private and cannot be referenced from a default argument value}}

    return 0
  }()) -> Int {
    get {}
    set {}
  }

  public subscript(y y: Int = privateIntFunction()) -> Int {
    // expected-error@-1 {{global function 'privateIntFunction()' is private and cannot be referenced from a default argument value}}
    get {}
    set {}
  }
}
