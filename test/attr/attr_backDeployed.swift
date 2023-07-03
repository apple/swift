// RUN: %target-typecheck-verify-swift -parse-as-library \
// RUN:   -define-availability "_myProject 2.0:macOS 12.0"

// REQUIRES: OS=macosx

// MARK: - Valid declarations

// Ok, top level functions
@backDeployed(before: macOS 12.0)
public func backDeployedTopLevelFunc() {}

// Ok, internal decls may be back deployed when @usableFromInline
@backDeployed(before: macOS 12.0)
@usableFromInline
internal func backDeployedUsableFromInlineTopLevelFunc() {}

// Ok, decls in a struct
public struct TopLevelStruct {
  @backDeployed(before: macOS 12.0)
  public init<T>(_ t: T) {}

  @backDeployed(before: macOS 12.0)
  public func backDeployedMethod() {}

  @backDeployed(before: macOS 12.0)
  public var backDeployedComputedProperty: Int { 98 }

  @backDeployed(before: macOS 12.0)
  public subscript(_ index: Int) -> Int { index }

  @backDeployed(before: macOS 12.0)
  public var readWriteProperty: Int {
    get { 42 }
    set(newValue) {}
  }

  @backDeployed(before: macOS 12.0)
  public subscript(at index: Int) -> Int {
    get { 42 }
    set(newValue) {}
  }

  public var explicitReadAndModify: Int {
    @backDeployed(before: macOS 12.0)
    _read { yield 42 }

    @backDeployed(before: macOS 12.0)
    _modify {}
  }
}

// Ok, decls in an enum
public enum TopLevelEnum {
  case a, b

  @backDeployed(before: macOS 12.0)
  public init?(from name: String) {
    switch name {
    case "a": self = .a
    case "b": self = .b
    default: return nil
    }
  }

  @backDeployed(before: macOS 12.0)
  public func backDeployedMethod() {}

  @backDeployed(before: macOS 12.0)
  public var name: String {
    switch self {
    case .a: return "a"
    case .b: return "b"
    }
  }
}

// Ok, final decls in a non-final class
public class TopLevelClass {
  var x: Int

  public init(x: Int) {
    self.x = x
  }

  @backDeployed(before: macOS 12.0)
  public convenience init() {
    self.init(x: 1)
  }

  public func hook() {}

  @backDeployed(before: macOS 12.0)
  final public func backDeployedFinalMethod() {}

  @backDeployed(before: macOS 12.0)
  final public var backDeployedFinalComputedProperty: Int { 98 }

  @backDeployed(before: macOS 12.0)
  public static func backDeployedStaticMethod() {}

  @backDeployed(before: macOS 12.0)
  public final class func backDeployedClassMethod() {}
}

// Ok, final decls in a non-final, derived class
public class DerivedTopLevelClass: TopLevelClass {
  @backDeployed(before: macOS 12.0)
  final public func backDeployedFinalMethodOnDerived() {}
}

// Ok, decls in a final class
final public class FinalTopLevelClass {
  var x: Int

  public init(x: Int) {
    self.x = x
  }

  @backDeployed(before: macOS 12.0)
  public convenience init() {
    self.init(x: 1)
  }

  @backDeployed(before: macOS 12.0)
  public func backDeployedMethod() {}

  @backDeployed(before: macOS 12.0)
  public var backDeployedComputedProperty: Int { 98 }
}

// Ok, final decls in a non-final actor
@available(SwiftStdlib 5.1, *)
public actor TopLevelActor {
  var x: Int

  public init(x: Int) {
    self.x = x
  }

  @backDeployed(before: macOS 12.0)
  public init() {
    self.init(x: 1)
  }

  @backDeployed(before: macOS 12.0)
  final public func method() {}

  @backDeployed(before: macOS 12.0)
  final public var computedProperty: Int { 98 }
}

// Ok, decls on a final actor
@available(SwiftStdlib 5.1, *)
final public actor FinalTopLevelActor {
  var x: Int

  public init(x: Int) {
    self.x = x
  }

  @backDeployed(before: macOS 12.0)
  public init() {
    self.init(x: 1)
  }

  @backDeployed(before: macOS 12.0)
  public func method() {}

  @backDeployed(before: macOS 12.0)
  public var computedProperty: Int { 98 }
}

// Ok, decls in extensions of public types
extension TopLevelStruct {
  @backDeployed(before: macOS 12.0)
  public func backDeployedExtensionMethod() {}
}

extension TopLevelClass {
  @backDeployed(before: macOS 12.0)
  final public func backDeployedExtensionMethod() {}
}

extension FinalTopLevelClass {
  @backDeployed(before: macOS 12.0)
  public func backDeployedExtensionMethod() {}
}

@available(SwiftStdlib 5.1, *)
extension TopLevelActor {
  @backDeployed(before: macOS 12.0)
  final public func backDeployedExtensionMethod() {}
}

@available(SwiftStdlib 5.1, *)
extension FinalTopLevelActor {
  @backDeployed(before: macOS 12.0)
  public func backDeployedExtensionMethod() {}
}

public protocol TopLevelProtocol {}

extension TopLevelProtocol {
  @backDeployed(before: macOS 12.0)
  public func backDeployedExtensionMethod() {}
}


// MARK: - Unsupported declaration kinds

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
public class CannotBackDeployClass {}

public class CannotBackDeployClassDesignatedInit {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' cannot be applied to a non-final initializer}}
  public init(x: Int) {}
}

public final class CannotBackDeployClassDeinit {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to deinitializer declarations}}
  deinit {}
}

// Ok, final decls in a non-final, derived class
public class CannotBackDeployOverride: TopLevelClass {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' cannot be combined with 'override'}}
  final public override func hook() {}
}

@available(SwiftStdlib 5.1, *)
public actor CannotBackDeployActorDesignatedInit {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' cannot be applied to a non-final initializer}}
  public init(x: Int) {}
}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
public struct CannotBackDeployStruct {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' must not be used on stored properties}}
  public var cannotBackDeployStoredProperty: Int = 83

  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' must not be used on stored properties}}
  public lazy var cannotBackDeployLazyStoredProperty: Int = 15
}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
public enum CannotBackDeployEnum {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
  case cannotBackDeployEnumCase
}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' must not be used on stored properties}}
public var cannotBackDeployTopLevelVar = 79

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
extension TopLevelStruct {}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
protocol CannotBackDeployProtocol {}

@available(SwiftStdlib 5.1, *)
@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' attribute cannot be applied to this declaration}}
public actor CannotBackDeployActor {}

public struct ConformsToTopLevelProtocol: TopLevelProtocol {
  public init() {}
}

@available(SwiftStdlib 5.1, *)
@backDeployed(before: macOS 12.0) // expected-warning {{'@backDeployed' is unsupported on a var with a 'some' return type}}
public var cannotBackDeployVarWithOpaqueResultType: some TopLevelProtocol {
  return ConformsToTopLevelProtocol()
}

@available(SwiftStdlib 5.1, *)
@backDeployed(before: macOS 12.0) // expected-warning {{'@backDeployed' is unsupported on a global function with a 'some' return type}}
public func cannotBackDeployFuncWithOpaqueResultType() -> some TopLevelProtocol {
  return ConformsToTopLevelProtocol()
}

// MARK: - Function body diagnostics

public struct FunctionBodyDiagnostics {
  public func publicFunc() {}
  @usableFromInline func usableFromInlineFunc() {}
  func internalFunc() {} // expected-note {{instance method 'internalFunc()' is not '@usableFromInline' or public}}
  fileprivate func fileprivateFunc() {} // expected-note {{instance method 'fileprivateFunc()' is not '@usableFromInline' or public}}
  private func privateFunc() {} // expected-note {{instance method 'privateFunc()' is not '@usableFromInline' or public}}

  @backDeployed(before: macOS 12.0)
  public func backDeployedMethod_macOS() {
    struct Nested {} // expected-error {{type 'Nested' cannot be nested inside a '@backDeployed' function}}

    publicFunc()
    usableFromInlineFunc()
    internalFunc() // expected-error {{instance method 'internalFunc()' is internal and cannot be referenced from a '@backDeployed' function}}
    fileprivateFunc() // expected-error {{instance method 'fileprivateFunc()' is fileprivate and cannot be referenced from a '@backDeployed' function}}
    privateFunc() // expected-error {{instance method 'privateFunc()' is private and cannot be referenced from a '@backDeployed' function}}
  }

  @backDeployed(before: iOS 13.0)
  public func backDeployedMethod_iOS() {
#if !os(iOS)
    // Since this function body is only back deployed on iOS, the statements
    // of the body in a #if !os(iOS) block should be considered resilient.
    struct Nested {}

    publicFunc()
    usableFromInlineFunc()
    internalFunc()
    fileprivateFunc()
    privateFunc()
#endif
  }
}


// MARK: - Incompatible declarations

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' may not be used on fileprivate declarations}}
fileprivate func filePrivateFunc() {}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' may not be used on private declarations}}
private func privateFunc() {}

@backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' may not be used on internal declarations}}
internal func internalFunc() {}

private struct PrivateTopLevelStruct {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' may not be used on private declarations}}
  public func effectivelyPrivateFunc() {}
}

public class TopLevelClass2 {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' cannot be applied to a non-final instance method}}
  public func nonFinalMethod() {}

  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' cannot be applied to a non-final class method}}
  public class func nonFinalClassMethod() {}
}

@backDeployed(before: macOS 12.0, macOS 13.0) // expected-error {{'@backDeployed' contains multiple versions for macOS}}
public func duplicatePlatformsFunc1() {}

@backDeployed(before: macOS 12.0)
@backDeployed(before: macOS 13.0) // expected-error {{'@backDeployed' contains multiple versions for macOS}}
public func duplicatePlatformsFunc2() {}

@backDeployed(before: macOS 12.0)
@_alwaysEmitIntoClient // expected-error {{'@_alwaysEmitIntoClient' cannot be applied to a back deployed global function}}
public func alwaysEmitIntoClientFunc() {}

@backDeployed(before: macOS 12.0)
@inlinable // Ok
public func inlinableFunc() {}

@backDeployed(before: macOS 12.0)
@_transparent // expected-error {{'@_transparent' cannot be applied to a back deployed global function}}
public func transparentFunc() {}


// MARK: - Attribute parsing

@backDeployed(before: macos 12.0, iOS 15.0) // expected-warning {{unknown platform 'macos' for attribute '@backDeployed'; did you mean 'macOS'?}} {{23-28=macOS}}
public func incorrectPlatformCaseFunc() {}

@backDeployed(before: mscos 12.0, iOS 15.0) // expected-warning {{unknown platform 'mscos' for attribute '@backDeployed'; did you mean 'macOS'?}} {{23-28=macOS}}
public func incorrectPlatformSimilarFunc() {}

@backDeployed(before: macOS 12.0, unknownOS 1.0) // expected-warning {{unknown platform 'unknownOS' for attribute '@backDeployed'}}
public func unknownOSFunc() {}

@backDeployed(before: @) // expected-error {{expected platform in '@backDeployed' attribute}}
public func badPlatformFunc1() {}

@backDeployed(before: @ 12.0) // expected-error {{expected platform in '@backDeployed' attribute}}
public func badPlatformFunc2() {}

@backDeployed(before: macOS) // expected-error {{expected version number in '@backDeployed' attribute}}
public func missingVersionFunc1() {}

@backDeployed(before: macOS 12.0, iOS) // expected-error {{expected version number in '@backDeployed' attribute}}
public func missingVersionFunc2() {}

@backDeployed(before: macOS, iOS) // expected-error 2{{expected version number in '@backDeployed' attribute}}
public func missingVersionFunc3() {}

@backDeployed(before: macOS 12.0, iOS 15.0,) // expected-error {{unexpected ',' separator}}
public func unexpectedSeparatorFunc() {}

@backDeployed(before: macOS 12.0.1) // expected-warning {{'@backDeployed' only uses major and minor version number}}
public func patchVersionFunc() {}

@backDeployed(before: macOS 12.0, * 9.0) // expected-warning {{* as platform name has no effect in '@backDeployed' attribute}}
public func wildcardWithVersionFunc() {}

@backDeployed(before: macOS 12.0, *) // expected-warning {{* as platform name has no effect in '@backDeployed' attribute}}
public func trailingWildcardFunc() {}

@backDeployed(before: macOS 12.0, *, iOS 15.0) // expected-warning {{* as platform name has no effect in '@backDeployed' attribute}}
public func embeddedWildcardFunc() {}

@backDeployed(before: _myProject 3.0) // expected-error {{reference to undefined version '3.0' for availability macro '_myProject'}}
public func macroVersioned() {}

@backDeployed(before: _myProject) // expected-error {{reference to undefined version '0' for availability macro '_myProject'}}
public func missingMacroVersion() {}

// Fall back to the default diagnostic when the macro is unknown.
@backDeployed(before: _unknownMacro) // expected-warning {{unknown platform '_unknownMacro' for attribute '@backDeployed'}}
// expected-error@-1 {{expected version number in '@backDeployed' attribute}}
public func unknownMacroMissingVersion() {}

@backDeployed(before: _unknownMacro 1.0) // expected-warning {{unknown platform '_unknownMacro' for attribute '@backDeployed'}}
// expected-error@-1 {{expected at least one platform version in '@backDeployed' attribute}}
public func unknownMacroVersioned() {}

@backDeployed(before: _unknownMacro 1.0, _myProject 2.0) // expected-warning {{unknown platform '_unknownMacro' for attribute '@backDeployed'}}
public func knownAndUnknownMacroVersioned() {}

@backDeployed() // expected-error {{expected 'before:' in '@backDeployed' attribute}}
// expected-error@-1 {{expected at least one platform version in '@backDeployed' attribute}}
public func emptyAttributeFunc() {}

@backDeployed(macOS 12.0) // expected-error {{expected 'before:' in '@backDeployed' attribute}} {{15-15=before:}}
public func missingBeforeFunc() {}

@backDeployed(before) // expected-error {{expected ':' after 'before' in '@backDeployed' attribute}} {{21-21=:}}
// expected-error@-1 {{expected at least one platform version in '@backDeployed' attribute}}
public func missingColonAfterBeforeFunc() {}

@backDeployed(before macOS 12.0) // expected-error {{expected ':' after 'before' in '@backDeployed' attribute}} {{21-21=:}}
public func missingColonBetweenBeforeAndPlatformFunc() {}

@backDeployed(before: macOS 12.0,) // expected-error {{unexpected ',' separator}} {{33-34=}}
public func unexpectedTrailingCommaFunc() {}

@backDeployed(before: macOS 12.0,, iOS 15.0) // expected-error {{unexpected ',' separator}} {{34-35=}}
public func extraCommaFunc() {}

@backDeployed(before:) // expected-error {{expected at least one platform version in '@backDeployed' attribute}}
public func emptyPlatformVersionsFunc() {}

@backDeployed // expected-error {{expected '(' in '@backDeployed' attribute}}
public func expectedLeftParenFunc() {}

@backDeployed(before: macOS 12.0 // expected-note {{to match this opening '('}}
public func expectedRightParenFunc() {} // expected-error {{expected ')' in '@backDeployed' argument list}}

// MARK: - Legacy attribute spelling

@_backDeploy(before: macOS 12.0)
public func legacyBackDeployFunc() {}
