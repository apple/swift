// RUN: %target-typecheck-verify-swift -swift-version 5
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-testing
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -enable-testing
@inlinable struct TestInlinableStruct {}
// expected-error@-1 {{'@inlinable' attribute cannot be applied to this declaration}}

@inlinable @usableFromInline func redundantAttribute() {}
// expected-warning@-1 {{'@usableFromInline' attribute has no effect on '@inlinable' global function 'redundantAttribute()'}} {{12-30=}}

private func privateFunction() {}
// expected-note@-1 2{{global function 'privateFunction()' is not '@usableFromInline' or public}}
fileprivate func fileprivateFunction() {}
// expected-note@-1{{global function 'fileprivateFunction()' is not '@usableFromInline' or public}}
func internalFunction() {}
// expected-note@-1 2{{global function 'internalFunction()' is not '@usableFromInline' or public}}
@usableFromInline func versionedFunction() {}
public func publicFunction() {}

private struct PrivateStruct {}
// expected-note@-1 5{{struct 'PrivateStruct' is not '@usableFromInline' or public}}
// expected-note@-2 2{{initializer 'init()' is not '@usableFromInline' or public}}
struct InternalStruct {}
// expected-note@-1 3{{struct 'InternalStruct' is not '@usableFromInline' or public}}
// expected-note@-2 {{initializer 'init()' is not '@usableFromInline' or public}}
@usableFromInline struct VersionedStruct {
  @usableFromInline init() {}
}
public struct PublicStruct {
  public init() {}

  @inlinable public var storedProperty: Int
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to stored properties}}

  @inlinable public lazy var lazyProperty: Int = 0
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to stored properties}}
}

public struct Struct {
  @_transparent
  public func publicTransparentMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside a '@_transparent' function}}

    publicFunction()
    // OK
    versionedFunction()
    // OK
    internalFunction()
    // expected-error@-1 {{global function 'internalFunction()' is internal and cannot be referenced from a '@_transparent' function}}
    fileprivateFunction()
    // expected-error@-1 {{global function 'fileprivateFunction()' is fileprivate and cannot be referenced from a '@_transparent' function}}
    privateFunction()
    // expected-error@-1 {{global function 'privateFunction()' is private and cannot be referenced from a '@_transparent' function}}
  }

  @inlinable
  public func publicInlinableMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inlinable' function}}

    let _: PublicStruct
    let _: VersionedStruct
    let _: InternalStruct
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    let _: PrivateStruct
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}

    let _ = PublicStruct.self
    let _ = VersionedStruct.self
    let _ = InternalStruct.self
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    let _ = PrivateStruct.self
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}

    let _ = PublicStruct()
    let _ = VersionedStruct()
    let _ = InternalStruct()
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    // expected-error@-2 {{initializer 'init()' is internal and cannot be referenced from an '@inlinable' function}}
    let _ = PrivateStruct()
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}
    // expected-error@-2 {{initializer 'init()' is private and cannot be referenced from an '@inlinable' function}}
  }

  private func privateMethod() {}
  // expected-note@-1 {{instance method 'privateMethod()' is not '@usableFromInline' or public}}

  @_transparent
  @usableFromInline
  func versionedTransparentMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside a '@_transparent' function}}
    privateMethod()
    // expected-error@-1 {{instance method 'privateMethod()' is private and cannot be referenced from a '@_transparent' function}}
  }

  @inlinable
  func internalInlinableMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inlinable' function}}
  }

  @_transparent
  func internalTransparentMethod() {
    struct Nested {}
    // OK
  }

  @inlinable
  private func privateInlinableMethod() {
  // expected-error@-2 {{'@inlinable' attribute can only be applied to public declarations, but 'privateInlinableMethod' is private}}
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inlinable' function}}
  }

  @inline(__always)
  func internalInlineAlwaysMethod() {
    struct Nested {}
    // OK
  }
}

// Make sure protocol extension members can reference protocol requirements
// (which do not inherit the @usableFromInline attribute).
@usableFromInline
protocol VersionedProtocol {
  associatedtype T

  func requirement() -> T
}

extension VersionedProtocol {
  func internalMethod() {}
  // expected-note@-1 {{instance method 'internalMethod()' is not '@usableFromInline' or public}}

  @inlinable
  func versionedMethod() -> T {
    internalMethod()
    // expected-error@-1 {{instance method 'internalMethod()' is internal and cannot be referenced from an '@inlinable' function}}

    return requirement()
  }
}

enum InternalEnum {
  // expected-note@-1 2{{enum 'InternalEnum' is not '@usableFromInline' or public}}
  // expected-note@-2 {{type declared here}}
  case apple
  // expected-note@-1 {{enum case 'apple' is not '@usableFromInline' or public}}
  case orange
  // expected-note@-1 {{enum case 'orange' is not '@usableFromInline' or public}}
}

@inlinable public func usesInternalEnum() {
  _ = InternalEnum.apple
  // expected-error@-1 {{enum 'InternalEnum' is internal and cannot be referenced from an '@inlinable' function}}
  // expected-error@-2 {{enum case 'apple' is internal and cannot be referenced from an '@inlinable' function}}
  let _: InternalEnum = .orange
  // expected-error@-1 {{enum 'InternalEnum' is internal and cannot be referenced from an '@inlinable' function}}
  // expected-error@-2 {{enum case 'orange' is internal and cannot be referenced from an '@inlinable' function}}
}

@usableFromInline enum VersionedEnum {
  case apple
  case orange
  case pear(InternalEnum)
  // expected-error@-1 {{type of enum case in '@usableFromInline' enum must be '@usableFromInline' or public}}
  case persimmon(String)
}

@inlinable public func usesVersionedEnum() {
  _ = VersionedEnum.apple
  let _: VersionedEnum = .orange
  _ = VersionedEnum.persimmon
}


// Inherited initializers - <rdar://problem/34398148>
@usableFromInline
@_fixed_layout
class Base {
  @usableFromInline
  init(x: Int) {}
}

@usableFromInline
@_fixed_layout
class Middle : Base {}

@usableFromInline
@_fixed_layout
class Derived : Middle {
  @inlinable
  init(y: Int) {
    super.init(x: y)
  }
}


// More inherited initializers
@_fixed_layout
public class Base2 {
  @inlinable
  public init(x: Int) {}
}

@_fixed_layout
@usableFromInline
class Middle2 : Base2 {}

@_fixed_layout
@usableFromInline
class Derived2 : Middle2 {
  @inlinable
  init(y: Int) {
    super.init(x: y)
  }
}


// https://github.com/apple/swift/issues/53331
// Even more inherited initializers.

@_fixed_layout
public class Base3 {}
// expected-note@-1 {{initializer 'init()' is not '@usableFromInline' or public}}

@_fixed_layout
public class Derived3 : Base3 {
  @inlinable
  public init(_: Int) {}
  // expected-error@-1 {{initializer 'init()' is internal and cannot be referenced from an '@inlinable' function}}
  // expected-note@-2 {{call to unavailable initializer 'init()' from superclass 'Base3' occurs implicitly at the end of this initializer}}
}

@_fixed_layout
public class Base4 {}

@_fixed_layout
@usableFromInline
class Middle4 : Base4 {}
// expected-note@-1 {{initializer 'init()' is not '@usableFromInline' or public}}

@_fixed_layout
@usableFromInline
class Derived4 : Middle4 {
  @inlinable
  public init(_: Int) {}
  // expected-error@-1 {{initializer 'init()' is internal and cannot be referenced from an '@inlinable' function}}
  // expected-note@-2 {{call to unavailable initializer 'init()' from superclass 'Middle4' occurs implicitly at the end of this initializer}}
}


// Stored property initializer expressions.
//
// Note the behavior here does not depend on the state of the -enable-library-evolution
// flag; the test runs with both the flag on and off. Only the explicit
// presence of a '@_fixed_layout' attribute determines the behavior here.

let internalGlobal = 0
// expected-note@-1 {{let 'internalGlobal' is not '@usableFromInline' or public}}
public let publicGlobal = 0

struct InternalStructWithInit {
  var x = internalGlobal // OK
  var y = publicGlobal // OK
}

public struct PublicResilientStructWithInit {
  var x = internalGlobal // OK
  var y = publicGlobal // OK
}

private func privateIntReturningFunc() -> Int { return 0 }
internal func internalIntReturningFunc() -> Int { return 0 }

@frozen
public struct PublicFixedStructWithInit {
  var x = internalGlobal // expected-error {{let 'internalGlobal' is internal and cannot be referenced from a property initializer in a '@frozen' type}}
  var y = publicGlobal // OK

  // Static property initializers are not inlinable contexts.
  static var z = privateIntReturningFunc() // OK
  static var a = internalIntReturningFunc() // OK

  // Test the same with a multi-statement closure, which introduces a
  // new DeclContext.
  static var zz: Int = {
    let x = privateIntReturningFunc()
    return x
  }()
  static var aa: Int = {
    let x = internalIntReturningFunc()
    return x
  }()
}

public struct KeypathStruct {
  var x: Int
  // expected-note@-1 {{property 'x' is not '@usableFromInline' or public}}

  @inlinable public func usesKeypath() {
    _ = \KeypathStruct.x
    // expected-error@-1 {{property 'x' is internal and cannot be referenced from an '@inlinable' function}}
  }
}

public struct HasInternalSetProperty {
  public internal(set) var x: Int // expected-note {{setter for property 'x' is not '@usableFromInline' or public}}

  @inlinable public mutating func setsX() {
    x = 10 // expected-error {{setter for property 'x' is internal and cannot be referenced from an '@inlinable' function}}
  }
}

@usableFromInline protocol P {
  typealias T = Int
}

extension P {
  @inlinable func f() {
    _ = T.self // ok, typealias inherits @usableFromInline from P
  }
}

// rdar://problem/60605117
public struct PrivateInlinableCrash {
  @inlinable // expected-error {{'@inlinable' attribute can only be applied to public declarations, but 'formatYesNo' is private}}
  private func formatYesNo(_ value: Bool) -> String {
    value ? "YES" : "NO"
  }
}

// https://github.com/apple/swift/issues/54842

@inlinable public func inlinableOuterFunction() {
  func innerFunction1(x: () = privateFunction()) {}
  // expected-error@-1 {{global function 'privateFunction()' is private and cannot be referenced from a default argument value}}

  func innerFunction2(x: () = internalFunction()) {}
  // expected-error@-1 {{global function 'internalFunction()' is internal and cannot be referenced from a default argument value}}

  func innerFunction3(x: () = versionedFunction()) {}

  func innerFunction4(x: () = publicFunction()) {}
}

// This is OK -- lazy property initializers are emitted inside the getter,
// which is never @inlinable.
@frozen public struct LazyField {
  public lazy var y: () = privateFunction()

  @inlinable private lazy var z: () = privateFunction()
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to stored properties}}
}

@inlinable public func nestedBraceStmtTest() {
  if true {
    let _: PrivateStruct = PrivateStruct()
    // expected-error@-1 2{{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}
    // expected-error@-2 {{initializer 'init()' is private and cannot be referenced from an '@inlinable' function}}
  }
}

// Just make sure we don't crash.
private func deferBodyTestCall() {} // expected-note {{global function 'deferBodyTestCall()' is not '@usableFromInline' or public}}
@inlinable public func deferBodyTest() {
  defer {
    deferBodyTestCall() // expected-error {{global function 'deferBodyTestCall()' is private and cannot be referenced from an '@inlinable' function}}
  }
  _ = ()
}
