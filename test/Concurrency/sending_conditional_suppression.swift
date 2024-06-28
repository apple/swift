// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -enable-upcoming-feature SendingArgsAndResults -swift-version 5 -enable-library-evolution -module-name test -emit-module -o %t/test.swiftmodule -emit-module-interface-path - -disable-availability-checking -Xllvm -swift-ast-printer-number-suppression-checks %s | %FileCheck %s

// REQUIRES: asserts

public class NonSendableKlass {}

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func transferArgTest(_ x: sending test.NonSendableKlass)
// CHECK-NEXT: #else
// CHECK-NEXT: public func transferArgTest(_ x: test.NonSendableKlass)
// CHECK-NEXT: #endif
public func transferArgTest(_ x: sending NonSendableKlass) {}

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func transferResultTest() -> sending test.NonSendableKlass
// CHECK-NEXT: #else
// CHECK-NEXT: public func transferResultTest() -> test.NonSendableKlass
// CHECK-NEXT: #endif
public func transferResultTest() -> sending NonSendableKlass { fatalError() }

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func transferArgAndResultTest(_ x: test.NonSendableKlass, _ y: sending test.NonSendableKlass, _ z: test.NonSendableKlass) -> sending test.NonSendableKlass
// CHECK-NEXT: #else
// CHECK-NEXT: public func transferArgAndResultTest(_ x: test.NonSendableKlass, _ y: test.NonSendableKlass, _ z: test.NonSendableKlass) -> test.NonSendableKlass
// CHECK-NEXT: #endif
public func transferArgAndResultTest(_ x: NonSendableKlass, _ y: sending NonSendableKlass, _ z: NonSendableKlass) -> sending NonSendableKlass { fatalError() }

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func argEmbeddedInType(_ fn: (sending test.NonSendableKlass) -> ())
// CHECK-NEXT: #else
// CHECK-NEXT: public func argEmbeddedInType(_ fn: (test.NonSendableKlass) -> ())
// CHECK-NEXT: #endif
public func argEmbeddedInType(_ fn: (sending NonSendableKlass) -> ()) {}

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func resultEmbeddedInType(_ fn: () -> sending test.NonSendableKlass)
// CHECK-NEXT: #else
// CHECK-NEXT: public func resultEmbeddedInType(_ fn: () -> test.NonSendableKlass)
// CHECK-NEXT: #endif
public func resultEmbeddedInType(_ fn: () -> sending NonSendableKlass) {}

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
// CHECK-NEXT: public func argAndResultEmbeddedInType(_ fn: (test.NonSendableKlass, sending test.NonSendableKlass, test.NonSendableKlass) -> sending test.NonSendableKlass)
// CHECK-NEXT: #else
// CHECK-NEXT: public func argAndResultEmbeddedInType(_ fn: (test.NonSendableKlass, test.NonSendableKlass, test.NonSendableKlass) -> test.NonSendableKlass)
// CHECK-NEXT: #endif
public func argAndResultEmbeddedInType(_ fn: (NonSendableKlass, sending NonSendableKlass, NonSendableKlass) -> sending NonSendableKlass) {}

public class TestInKlass {
  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassArg(_ x: sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassArg(_ x: test.NonSendableKlass)
  // CHECK-NEXT: #endif
  public func testKlassArg(_ x: sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassResult() -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassResult() -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  public func testKlassResult() -> sending NonSendableKlass { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassArgAndResult(_ x: test.NonSendableKlass, _ y: sending test.NonSendableKlass, z: test.NonSendableKlass) -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassArgAndResult(_ x: test.NonSendableKlass, _ y: test.NonSendableKlass, z: test.NonSendableKlass) -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  public func testKlassArgAndResult(_ x: NonSendableKlass, _ y: sending NonSendableKlass, z: NonSendableKlass) -> sending NonSendableKlass { fatalError() }
}

public struct TestInStruct {
  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassArg(_ x: sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassArg(_ x: test.NonSendableKlass)
  // CHECK-NEXT: #endif
  public func testKlassArg(_ x: sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassResult() -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassResult() -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  public func testKlassResult() -> sending NonSendableKlass { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testKlassArgAndResult(_ x: test.NonSendableKlass, _ y: sending test.NonSendableKlass, z: test.NonSendableKlass) -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testKlassArgAndResult(_ x: test.NonSendableKlass, _ y: test.NonSendableKlass, z: test.NonSendableKlass) -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  public func testKlassArgAndResult(_ x: NonSendableKlass, _ y: sending NonSendableKlass, z: NonSendableKlass) -> sending NonSendableKlass { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testFunctionArg(_ x: () -> sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testFunctionArg(_ x: () -> test.NonSendableKlass)
  // CHECK-NEXT: #endif
  public func testFunctionArg(_ x: () -> sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public func testFunctionResult() -> (() -> sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: public func testFunctionResult() -> (() -> test.NonSendableKlass)
  // CHECK-NEXT: #endif
  public func testFunctionResult() -> (() -> sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassArg(_ x: sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassArg(_ x: test.NonSendableKlass)
  // CHECK-NEXT: #endif
  @usableFromInline func testUsableFromInlineKlassArg(_ x: sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassResult() -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassResult() -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  @usableFromInline
  func testUsableFromInlineKlassResult() -> sending NonSendableKlass { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassArgAndResult(_ x: test.NonSendableKlass, _ y: sending test.NonSendableKlass, z: test.NonSendableKlass) -> sending test.NonSendableKlass
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineKlassArgAndResult(_ x: test.NonSendableKlass, _ y: test.NonSendableKlass, z: test.NonSendableKlass) -> test.NonSendableKlass
  // CHECK-NEXT: #endif
  @usableFromInline
  func testUsableFromInlineKlassArgAndResult(_ x: NonSendableKlass, _ y: sending NonSendableKlass, z: NonSendableKlass) -> sending NonSendableKlass { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineFunctionArg(_ x: () -> sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineFunctionArg(_ x: () -> test.NonSendableKlass)
  // CHECK-NEXT: #endif
  @usableFromInline
  func testUsableFromInlineFunctionArg(_ x: () -> sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineFunctionResult() -> (() -> sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal func testUsableFromInlineFunctionResult() -> (() -> test.NonSendableKlass)
  // CHECK-NEXT: #endif
  @usableFromInline
  func testUsableFromInlineFunctionResult() -> (() -> sending NonSendableKlass) { fatalError() }

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public var publicVarFieldFunctionArg: (sending test.NonSendableKlass) -> ()
  // CHECK-NEXT: #else
  // CHECK-NEXT: public var publicVarFieldFunctionArg: (test.NonSendableKlass) -> ()
  // CHECK-NEXT: #endif
  public var publicVarFieldFunctionArg: (sending NonSendableKlass) -> ()

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal var internalVarFieldFunctionArg: (sending test.NonSendableKlass) -> ()
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal var internalVarFieldFunctionArg: (test.NonSendableKlass) -> ()
  // CHECK-NEXT: #endif
  @usableFromInline
  var internalVarFieldFunctionArg: (sending NonSendableKlass) -> ()

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: public let publicLetFieldFunctionArg: (sending test.NonSendableKlass) -> ()
  // CHECK-NEXT: #else
  // CHECK-NEXT: public let publicLetFieldFunctionArg: (test.NonSendableKlass) -> ()
  // CHECK-NEXT: #endif
  public let publicLetFieldFunctionArg: (sending NonSendableKlass) -> ()

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal let internalLetFieldFunctionArg: (sending test.NonSendableKlass) -> ()
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal let internalLetFieldFunctionArg: (test.NonSendableKlass) -> ()
  // CHECK-NEXT: #endif
  @usableFromInline
  let internalLetFieldFunctionArg: (sending NonSendableKlass) -> ()

  // CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal init(_ x: Swift.Int, transformWithResult: @escaping () async throws -> sending test.NonSendableKlass)
  // CHECK-NEXT: #else
  // CHECK-NEXT: @usableFromInline
  // CHECK-NEXT: internal init(_ x: Swift.Int, transformWithResult: @escaping () async throws -> test.NonSendableKlass)
  // CHECK-NEXT: #endif
  @usableFromInline
  internal init(_ x: Int, transformWithResult: @escaping () async throws -> sending NonSendableKlass) { fatalError() }
}

// Make sure that we emit compiler(>= 5.3) when emitting the suppressing check
// to make sure we do not fail if we fail to parse sending in the if block.

// CHECK-LABEL: #if compiler(>=5.3) && $OptionalIsolatedParameters && $ExpressionMacroDefaultArguments // Suppression Count: 24
// CHECK-NEXT: #if compiler(>=5.3) && $SendingArgsAndResults // Suppression Count: 25
// CHECK-NEXT: @inlinable public func withCheckedContinuation<T>(isolation: isolated (any _Concurrency.Actor)? = #isolation, function: Swift.String = #function, _ body: (_Concurrency.CheckedContinuation<T, Swift.Never>) -> Swift.Void) async -> sending T {
// CHECK-NEXT:   fatalError()
// CHECK-NEXT: }
// CHECK-NEXT: #else
// CHECK-NEXT: @inlinable public func withCheckedContinuation<T>(isolation: isolated (any _Concurrency.Actor)? = #isolation, function: Swift.String = #function, _ body: (_Concurrency.CheckedContinuation<T, Swift.Never>) -> Swift.Void) async -> T {
// CHECK-NEXT:   fatalError()
// CHECK-NEXT: }
// CHECK-NEXT: #endif
// CHECK-NEXT: #endif
@inlinable public func withCheckedContinuation<T>(
  isolation: isolated (any _Concurrency.Actor)? = #isolation,
  function: String = #function,
  _ body: (_Concurrency.CheckedContinuation<T, Swift.Never>) -> Swift.Void
) async -> sending T {
  fatalError()
}

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults // Suppression Count: 26
// CHECK-NEXT: public var publicGlobal: (sending test.NonSendableKlass) -> ()
// CHECK-NEXT: #else
// CHECK-NEXT: public var publicGlobal: (test.NonSendableKlass) -> ()
// CHECK-NEXT: #endif
public var publicGlobal: (sending NonSendableKlass) -> () = { x in fatalError() }

// CHECK-LABEL: #if compiler(>=5.3) && $SendingArgsAndResults // Suppression Count: 27
// CHECK-NEXT: @usableFromInline
// CHECK-NEXT: internal var usableFromInlineGlobal: (sending test.NonSendableKlass) -> ()
// CHECK-NEXT: #else
// CHECK-NEXT: @usableFromInline
// CHECK-NEXT: internal var usableFromInlineGlobal: (test.NonSendableKlass) -> ()
// CHECK-NEXT: #endif
@usableFromInline
internal var usableFromInlineGlobal: (sending NonSendableKlass) -> () = { x in fatalError() }
