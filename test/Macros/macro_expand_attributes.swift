// REQUIRES: swift_swift_parser, executable_test

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -module-name MacroUser -DTEST_DIAGNOSTICS -swift-version 5
// RUN: %target-build-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) %s -o %t/main -module-name MacroUser -swift-version 5
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

@attached(memberAttribute) macro wrapAllProperties() = #externalMacro(module: "MacroDefinition", type: "WrapAllProperties")

@attached(memberAttribute)
public macro wrapStoredProperties(_ attributeName: String) = #externalMacro(module: "MacroDefinition", type: "WrapStoredPropertiesMacro")

@propertyWrapper
struct Wrapper<T> {
  init(wrappedValue: T) {
    print("initializing wrapper with \(wrappedValue)")
    self.wrappedValue = wrappedValue
  }
  var wrappedValue: T {
    willSet { print("setting \(newValue)") }
  }
}

protocol P {
  func reading<EnclosingSelf, Value>(from keyPath: KeyPath<EnclosingSelf, Value>)
  func writing<EnclosingSelf, Value>(to keyPath: KeyPath<EnclosingSelf, Value>)
}

extension P {
  func reading<EnclosingSelf, Value>(from keyPath: KeyPath<EnclosingSelf, Value>) {
    print("reading from key-path")
  }

  func writing<EnclosingSelf, Value>(to keyPath: KeyPath<EnclosingSelf, Value>) {
    print("writing to key-path")
  }
}

@propertyWrapper
struct EnclosingSelfWrapper<Value> {
  private var stored: Value

  init(wrappedValue: Value) { self.stored = wrappedValue }

  @available(*, unavailable)
  var wrappedValue: Value {
    get { fatalError() }
    set { fatalError() }
  }

  static subscript<EnclosingSelf: P>(
    _enclosingInstance observed: EnclosingSelf,
    wrapped wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Value>,
    storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Self>
  ) -> Value {
    get {
      observed.reading(from: wrappedKeyPath)
      return observed[keyPath: storageKeyPath].stored
    }
    set {
      observed.writing(to: wrappedKeyPath)
      observed[keyPath: storageKeyPath].stored = newValue

    }
  }
}

@wrapAllProperties
struct S {
  var x: Int
  var y: Int
}

// CHECK: initializing wrapper with 0
// CHECK: initializing wrapper with 1
var s = S(x: 0, y: 1)

// CHECK: setting 10
s.x = 10

// CHECK: setting 100
s.y = 100


@wrapAllProperties
class C: P {
  var x: Int = 0
  var y: Int = 0
}

var c = C()
// CHECK: reading from key-path
_ = c.x
// CHECK: reading from key-path
_ = c.y

// CHECK: writing to key-path
c.x = 10
// CHECK: writing to key-path
c.y = 100

// Use the "wrapStoredProperties" macro to deprecate all of the stored
// properties.
@wrapStoredProperties(#"available(*, deprecated, message: "hands off my data")"#)
struct OldStorage {
  var x: Int
}

// The deprecation warning below comes from the deprecation attribute
// introduced by @wrapStoredProperties on OldStorage.
_ = OldStorage(x: 5).x   // expected-warning{{'x' is deprecated: hands off my data}}

@wrapStoredProperties(#"available(*, deprecated, message: "hands off my data")"#)
class C2: P {
  var x: Int = 0
  var y: Int = 0

  var squareOfLength: Int {
    return x*x + y*y // expected-warning 4{{hands off my data}}
  }

  var blah: Int { squareOfLength }
}

@attached(member, names: named(expandedMember))
macro AddMember() = #externalMacro(module: "MacroDefinition", type: "SingleMemberMacro")

@AddMember
@wrapAllProperties
struct TestExpansionOrder {
  var originalMember: Int = 10
}

var expansionOrder = TestExpansionOrder()

// CHECK-NOT: setting 27
expansionOrder.expandedMember = 27

// CHECK: setting 28
expansionOrder.originalMember = 28

#if TEST_DIAGNOSTICS
@wrapAllProperties
typealias A = Int
// expected-error@-1{{'memberAttribute' macro cannot be attached to type alias}}

@wrapAllProperties
func noMembers() {}
// expected-error@-1{{'memberAttribute' macro cannot be attached to global function}}
#endif
