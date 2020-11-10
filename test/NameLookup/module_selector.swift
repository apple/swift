// RUN: %target-typecheck-verify-swift -sdk %clang-importer-sdk -module-name main -I %S/Inputs -enable-experimental-module-selector

// FIXME: This only works with ASTScopes, and we might not care about that by the time this feature is ready.
// RUN-DISABLED: %target-typecheck-verify-swift -sdk %clang-importer-sdk -module-name main -I %S/Inputs -enable-experimental-module-selector -disable-astscope-lookup

// Make sure the lack of the experimental flag disables the feature:
// RUN: not %target-typecheck-verify-swift -sdk %clang-importer-sdk -module-name main -I %S/Inputs 2>/dev/null

// FIXME: This test doesn't really cover:
//
// * Whether X::foo finds foos in X's re-exports
// * Whether we handle access paths correctly
// * Interaction with ClangImporter
// * Cross-module overlays, when those happen
//
// It also might not cover all combinations of name lookup paths and inputs.

import ModuleSelectorTestingKit

import ctypes::bits
import struct ModuleSelectorTestingKit::A

let magnitude: Never = fatalError()

// Test correct code using `A`

extension ModuleSelectorTestingKit::A: Swift::Equatable {
  @_implements(Swift::Equatable, ==(_:_:))
  public static func equals(_: ModuleSelectorTestingKit::A, _: ModuleSelectorTestingKit::A) -> Swift::Bool {
    Swift::fatalError()
  }

  // FIXME: Add tests with autodiff @_differentiable(jvp:vjp:) and
  // @_derivative(of:)

  @_dynamicReplacement(for: ModuleSelectorTestingKit::negate())
  mutating func myNegate() {
    let fn: (Swift::Int, Swift::Int) -> Swift::Int =
      (+)
      // FIXME: it'd be nice to handle module selectors on operators.

    let magnitude: Int.Swift::Magnitude = main::magnitude
    // expected-error@-1 {{cannot convert value of type 'Never' to specified type 'Int.Magnitude' (aka 'UInt')}}

    if Swift::Bool.Swift::random() {
      self.ModuleSelectorTestingKit::negate()
    }
    else {
      self = ModuleSelectorTestingKit::A(value: .Swift::min)
    }

    self.main::myNegate()
  }

  // FIXME: Can we test @convention(witness_method:)?
}

// Test resolution of main:: using `B`

extension main::B {}
// expected-error@-1 {{type 'B' is not imported through module 'main'}}
// expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{11-15=ModuleSelectorTestingKit}}

extension B: main::Equatable {
  // expected-error@-1 {{type 'Equatable' is not imported through module 'main'}}
  // expected-note@-2 {{did you mean module 'Swift'?}} {{14-18=Swift}}

  @_implements(main::Equatable, main::==(_:_:))
  // expected-error@-1 {{name cannot be qualified with module selector here}} {{33-39=}}
  // expected-error@-2 {{type 'Equatable' is not imported through module 'main'}}
  // expected-note@-3 {{did you mean module 'Swift'?}} {{16-20=Swift}}
  public static func equals(_: main::B, _: main::B) -> main::Bool {
  // expected-error@-1 2{{type 'B' is not imported through module 'main'}}
  // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{32-36=ModuleSelectorTestingKit}}
  // expected-note@-3 {{did you mean module 'ModuleSelectorTestingKit'?}} {{44-48=ModuleSelectorTestingKit}}
  // expected-error@-4 {{type 'Bool' is not imported through module 'main'}}
  // expected-note@-5 {{did you mean module 'Swift'?}} {{56-60=Swift}}
    main::fatalError()
    // expected-EVENTUALLY-error@-1 {{declaration 'fatalError' is not imported through module 'main'}}
    // expected-EVENTUALLY-note@-2 {{did you mean module 'Swift'?}} {{4-8=Swift}}
  }

  // FIXME: Add tests with autodiff @_differentiable(jvp:vjp:) and
  // @_derivative(of:)
  
  @_dynamicReplacement(for: main::negate())
  // FIXME improve: expected-error@-1 {{replaced function 'main::negate()' could not be found}}
  mutating func myNegate() {
    let fn: (main::Int, main::Int) -> main::Int =
    // expected-error@-1 3{{type 'Int' is not imported through module 'main'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{14-18=Swift}}
    // expected-note@-3 {{did you mean module 'Swift'?}} {{25-29=Swift}}
    // expected-note@-4 {{did you mean module 'Swift'?}} {{39-43=Swift}}
      (main::+)
      // FIXME: it'd be nice to handle module selectors on operators.
      // expected-error@-2 {{expected expression}}
      // expected-error@-3 {{expected expression after operator}}

    let magnitude: Int.main::Magnitude = main::magnitude
    // expected-error@-1 {{type 'Magnitude' is not imported through module 'main'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{24-28=Swift}}

    if main::Bool.main::random() {
    // expected-error@-1 {{declaration 'Bool' is not imported through module 'main'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{8-12=Swift}}

      main::negate()
      // expected-error@-1 {{declaration 'negate' is not imported through module 'main'}}
      // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{7-11=self.ModuleSelectorTestingKit}}
    }
    else {
      self = main::B(value: .main::min)
      // expected-error@-1 {{declaration 'B' is not imported through module 'main'}}
      // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{14-18=ModuleSelectorTestingKit}}
    }
    
    self.main::myNegate()
  }

  // FIXME: Can we test @convention(witness_method:)?
}

// Test resolution of ModuleSelectorTestingKit:: using `C`

extension C {}

extension ModuleSelectorTestingKit::C: ModuleSelectorTestingKit::Equatable {
// expected-error@-1 {{type 'Equatable' is not imported through module 'ModuleSelectorTestingKit'}}
// expected-note@-2 {{did you mean module 'Swift'?}} {{40-64=Swift}}

  @_implements(ModuleSelectorTestingKit::Equatable, ModuleSelectorTestingKit::==(_:_:))
  // expected-error@-1 {{name cannot be qualified with module selector here}} {{53-79=}}
  // expected-error@-2 {{type 'Equatable' is not imported through module 'ModuleSelectorTestingKit'}}
  // expected-note@-3 {{did you mean module 'Swift'?}} {{16-40=Swift}}

  public static func equals(_: ModuleSelectorTestingKit::C, _: ModuleSelectorTestingKit::C) -> ModuleSelectorTestingKit::Bool {
  // expected-error@-1 {{type 'Bool' is not imported through module 'ModuleSelectorTestingKit'}}
  // expected-note@-2 {{did you mean module 'Swift'?}} {{96-120=Swift}}
    ModuleSelectorTestingKit::fatalError()
    // expected-EVENTUALLY-error@-1 {{type 'fatalError' is not imported through module 'main'}}
    // expected-EVENTUALLY-note@-2 {{did you mean module 'Swift'?}} {{4-8=Swift}}
  }

  // FIXME: Add tests with autodiff @_differentiable(jvp:vjp:) and
  // @_derivative(of:)
  
  @_dynamicReplacement(for: ModuleSelectorTestingKit::negate())
  mutating func myNegate() {
  // FIXME improve: expected-note@-1 {{did you mean 'myNegate'?}}

    let fn: (ModuleSelectorTestingKit::Int, ModuleSelectorTestingKit::Int) -> ModuleSelectorTestingKit::Int =
    // expected-error@-1 3{{type 'Int' is not imported through module 'ModuleSelectorTestingKit'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{14-38=Swift}}
    // expected-note@-3 {{did you mean module 'Swift'?}} {{45-69=Swift}}
    // expected-note@-4 {{did you mean module 'Swift'?}} {{79-103=Swift}}
      (ModuleSelectorTestingKit::+)
      // FIXME: it'd be nice to handle module selectors on operators.
      // expected-error@-2 {{expected expression}}
      // expected-error@-3 {{expected expression after operator}}

    let magnitude: Int.ModuleSelectorTestingKit::Magnitude = ModuleSelectorTestingKit::magnitude
    // expected-error@-1 {{type 'Magnitude' is not imported through module 'ModuleSelectorTestingKit'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{24-48=Swift}}

    if ModuleSelectorTestingKit::Bool.ModuleSelectorTestingKit::random() {
    // expected-error@-1 {{declaration 'Bool' is not imported through module 'ModuleSelectorTestingKit'}}
    // expected-note@-2 {{did you mean module 'Swift'?}} {{8-32=Swift}}

      ModuleSelectorTestingKit::negate()
      // expected-error@-1 {{declaration 'negate' is not imported through module 'ModuleSelectorTestingKit'}}
      // expected-note@-2 {{did you mean the member of 'self'?}} {{7-7=self.}}
    }
    else {
      self = ModuleSelectorTestingKit::C(value: .ModuleSelectorTestingKit::min)
      // FIXME improve: expected-error@-1 {{type 'Int' has no member 'ModuleSelectorTestingKit::min'}}
    }
    
    self.ModuleSelectorTestingKit::myNegate()
    // FIXME improve: expected-error@-1 {{value of type 'C' has no member 'ModuleSelectorTestingKit::myNegate'}}
  }

  // FIXME: Can we test @convention(witness_method:)?
}

// Test resolution of Swift:: using `D`
// FIXME: Many more of these should fail once the feature is actually implemented.

extension Swift::D {}
// expected-error@-1 {{type 'D' is not imported through module 'Swift'}}
// expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{11-16=ModuleSelectorTestingKit}}

extension D: Swift::Equatable {
// FIXME wat: expected-error@-1 *{{extension outside of file declaring struct 'D' prevents automatic synthesis of '==' for protocol 'Equatable'}}

  @_implements(Swift::Equatable, Swift::==(_:_:))
  // expected-error@-1 {{name cannot be qualified with module selector here}} {{34-41=}}

  public static func equals(_: Swift::D, _: Swift::D) -> Swift::Bool {
  // expected-error@-1 2{{type 'D' is not imported through module 'Swift'}}
  // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{32-37=ModuleSelectorTestingKit}}
  // expected-note@-3 {{did you mean module 'ModuleSelectorTestingKit'?}} {{45-50=ModuleSelectorTestingKit}}
    Swift::fatalError()
  }

  // FIXME: Add tests with autodiff @_differentiable(jvp:vjp:) and
  // @_derivative(of:)
  
  @_dynamicReplacement(for: Swift::negate())
  // FIXME improve: expected-error@-1 {{replaced function 'Swift::negate()' could not be found}}
  mutating func myNegate() {
  // FIXME improve: expected-note@-1 {{did you mean 'myNegate'?}}

    let fn: (Swift::Int, Swift::Int) -> Swift::Int =
    // FIXME:
      (Swift::+)
      // expected-error@-1 {{cannot convert value of type '()' to specified type '(Int, Int) -> Int'}}
      // expected-error@-2 {{expected expression}}
      // expected-error@-3 {{expected expression after operator}}
    let magnitude: Int.Swift::Magnitude = Swift::magnitude
    // expected-error@-1 {{declaration 'magnitude' is not imported through module 'Swift'}}
    // FIXME should be un-addressable via main: expected-note@-2 {{did you mean module 'main'?}}
    if Swift::Bool.Swift::random() {
      Swift::negate()
      // expected-error@-1 {{declaration 'negate' is not imported through module 'Swift'}}
      // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{7-12=self.ModuleSelectorTestingKit}}
    }
    else {
      self = Swift::D(value: .ModuleSelectorTestingKit::min)
      // expected-error@-1 {{declaration 'D' is not imported through module 'Swift'}}
      // expected-note@-2 {{did you mean module 'ModuleSelectorTestingKit'?}} {{14-19=ModuleSelectorTestingKit}}
    }
    
    self.Swift::myNegate()
    // FIXME improve: expected-error@-1 {{value of type 'D' has no member 'Swift::myNegate'}}
  }

  // FIXME: Can we test @convention(witness_method:)?
}

struct AvailableUser {
  @available(macOS 10.15, *) var use1: String { "foo" }

  @main::available() var use2
  // FIXME improve: expected-error@-1 {{unknown attribute 'available'}}
  // FIXME suppress: expected-error@-2 {{type annotation missing in pattern}}

  @ModuleSelectorTestingKit::available() var use4
  // no-error

  @Swift::available() var use5
  // FIXME improve: expected-error@-1 {{unknown attribute 'available'}}
  // FIXME suppress: expected-error@-2 {{type annotation missing in pattern}}
}

func builderUser2(@main::MyBuilder fn: () -> Void) {}
// FIXME improve: expected-error@-1 {{unknown attribute 'MyBuilder'}}

func builderUser3(@ModuleSelectorTestingKit::MyBuilder fn: () -> Void) {}
// no-error

func builderUser4(@Swift::MyBuilder fn: () -> Void) {}
// FIXME improve: expected-error@-1 {{unknown attribute 'MyBuilder'}}

func whitespace() {
  Swift::print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift:: print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift ::print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift :: print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift::
  print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift
  ::print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift ::
  print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift
  :: print
  // expected-error@-1 {{expression resolves to an unused function}}

  Swift
  ::
  print
  // expected-error@-1 {{expression resolves to an unused function}}
}

// Error cases

func main::decl1(
  // expected-error@-1 {{name of function declaration cannot be qualified with module selector}}
  main::p1: main::A,
  // expected-error@-1 {{argument label cannot be qualified with module selector}}
  // FIXME access path: expected-error@-2 {{type 'A' is not imported through module 'main'}}
  // FIXME access path: expected-note@-3 {{did you mean module 'ModuleSelectorTestingKit'?}} {{13-17=ModuleSelectorTestingKit}}
  main::label p2: main::A,
  // expected-error@-1 {{argument label cannot be qualified with module selector}}
  // FIXME access path: expected-error@-2 {{type 'A' is not imported through module 'main'}}
  // FIXME access path: expected-note@-3 {{did you mean module 'ModuleSelectorTestingKit'?}} {{19-23=ModuleSelectorTestingKit}}
  label main::p3: main::A
  // expected-error@-1 {{name of parameter declaration cannot be qualified with module selector}}
  // FIXME access path: expected-error@-2 {{type 'A' is not imported through module 'main'}}
  // FIXME access path: expected-note@-3 {{did you mean module 'ModuleSelectorTestingKit'?}} {{19-23=ModuleSelectorTestingKit}}
) {
  let main::decl1a = "a"
  // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}

  var main::decl1b = "b"
  // expected-error@-1 {{name of variable declaration cannot be qualified with module selector}}

  let (main::decl1c, main::decl1d) = ("c", "d")
  // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
  // expected-error@-2 {{name of constant declaration cannot be qualified with module selector}}

  if let (main::decl1e, main::decl1f) = Optional(("e", "f")) {}
  // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
  // expected-error@-2 {{name of constant declaration cannot be qualified with module selector}}

  guard let (main::decl1g, main::decl1h) = Optional(("g", "h")) else { return }
  // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
  // expected-error@-2 {{name of constant declaration cannot be qualified with module selector}}

  // From uses in the switch statements below:
  // expected-note@-5 3{{did you mean the local declaration?}}

  switch Optional(main::decl1g) {
  // expected-error@-1 {{declaration 'decl1g' is not imported through module 'main'}}
  case Optional.some(let main::decl1i):
    // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
    break
  case .none:
    break
  }

  switch Optional(main::decl1g) {
  // expected-error@-1 {{declaration 'decl1g' is not imported through module 'main'}}
  case let Optional.some(main::decl1j):
    // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
    break
  case .none:
    break
  }

  switch Optional(main::decl1g) {
 // expected-error@-1 {{declaration 'decl1g' is not imported through module 'main'}}
 case let main::decl1k?:
    // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}
    break
  case .none:
    break
  }

  for main::decl1l in "lll" {}
  // expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}

  "lll".forEach { [main::magnitude]
    // expected-error@-1 {{name of captured variable declaration cannot be qualified with module selector}}
    main::elem in print(elem)
    // expected-error@-1 {{name of parameter declaration cannot be qualified with module selector}}
  }

  "lll".forEach { (main::elem) in print(elem) }
  // expected-error@-1 {{name of parameter declaration cannot be qualified with module selector}}

  "lll".forEach { (main::elem) -> Void in print(elem) }
  // expected-error@-1 {{name of parameter declaration cannot be qualified with module selector}}

  "lll".forEach { (main::elem: Character) -> Void in print(elem) }
  // expected-error@-1 {{name of parameter declaration cannot be qualified with module selector}}
}
enum main::decl2 {
  // expected-error@-1 {{name of enum declaration cannot be qualified with module selector}}

  case main::decl2a
  // expected-error@-1 {{name of enum 'case' declaration cannot be qualified with module selector}}
}

struct main::decl3 {}
// expected-error@-1 {{name of struct declaration cannot be qualified with module selector}}

class main::decl4<main::T> {}
// expected-error@-1 {{name of class declaration cannot be qualified with module selector}}
// expected-error@-2 {{name of generic parameter declaration cannot be qualified with module selector}}

typealias main::decl5 = main::Bool
// expected-error@-1 {{name of typealias declaration cannot be qualified with module selector}}
// expected-error@-2 {{type 'Bool' is not imported through module 'main'}}
// expected-note@-3 {{did you mean module 'Swift'?}} {{25-29=Swift}}

protocol main::decl6 {
  // expected-error@-1 {{name of protocol declaration cannot be qualified with module selector}}

  associatedtype main::decl6a
  // expected-error@-1 {{name of associatedtype declaration cannot be qualified with module selector}}
}

let main::decl7 = 7
// expected-error@-1 {{name of constant declaration cannot be qualified with module selector}}

var main::decl8 = 8 {
// expected-error@-1 {{name of variable declaration cannot be qualified with module selector}}

  willSet(main::newValue) {}
  // expected-error@-1 {{name of accessor parameter declaration cannot be qualified with module selector}}

  didSet(main::oldValue) {}
  // expected-error@-1 {{name of accessor parameter declaration cannot be qualified with module selector}}
}

struct Parent {
  func main::decl1() {}
  // expected-error@-1 {{name of function declaration cannot be qualified with module selector}}

  enum main::decl2 {
  // expected-error@-1 {{name of enum declaration cannot be qualified with module selector}}

    case main::decl2a
    // expected-error@-1 {{name of enum 'case' declaration cannot be qualified with module selector}}
  }

  struct main::decl3 {}
  // expected-error@-1 {{name of struct declaration cannot be qualified with module selector}}

  class main::decl4 {}
  // expected-error@-1 {{name of class declaration cannot be qualified with module selector}}

  typealias main::decl5 = main::Bool
  // expected-error@-1 {{name of typealias declaration cannot be qualified with module selector}}
  // expected-error@-2 {{type 'Bool' is not imported through module 'main'}}
  // expected-note@-3 {{did you mean module 'Swift'?}} {{27-31=Swift}}
}

@_swift_native_objc_runtime_base(main::BaseClass)
// expected-error@-1 {{Objective-C class name in @_swift_native_objc_runtime_base}}
class C1 {}

infix operator <<<<< : Swift::AdditionPrecedence
// expected-error@-1 {{precedence group specifier cannot be qualified with module selector}}

precedencegroup main::PG1 {
// expected-error@-1 {{name of precedence group declaration cannot be qualified with module selector}}

  higherThan: Swift::AdditionPrecedence
  // expected-error@-1 {{precedence group specifier cannot be qualified with module selector}}
}

func badModuleNames() {
  NonexistentModule::print()
  // expected-error@-1 {{declaration 'print' is not imported through module 'NonexistentModule'}}
  // expected-note@-2 {{did you mean module 'Swift'?}} {{3-20=Swift}}
  // FIXME redundant: expected-note@-3  {{did you mean module 'Swift'?}}

  _ = "foo".NonexistentModule::count
  // FIXME improve: expected-error@-1 {{value of type 'String' has no member 'NonexistentModule::count'}}

  let x: NonexistentModule::MyType = NonexistentModule::MyType()
  // expected-error@-1 {{cannot find type 'NonexistentModule::MyType' in scope}}

  let y: A.NonexistentModule::MyChildType = fatalError()
  // expected-error@-1 {{'NonexistentModule::MyChildType' is not a member type of 'A'}}
}
