// A module with library evolution enabled has two different "minimum versions".
// One, the minimum deployment target, is the lowest version that non-ABI
// declarations and bodies of non-inlinable functions will ever see. The other,
// the minimum inlining target, is the lowest version that ABI declarations and
// inlinable bodies will ever see.
//
// Test that we use the right version floor in the right places.

// REQUIRES: OS=macosx

// Primary execution of this test. Uses the default minimum inlining version,
// which is the version when Swift was introduced.
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -parse-as-library -module-name Test -target %target-next-stable-abi-triple -target-min-inlining-version min


// Check that `-library-level api` implies `-target-min-inlining-version min`
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -parse-as-library -module-name Test -target %target-next-stable-abi-triple -library-level api -require-explicit-availability=ignore


// Check that these rules are only applied when requested and that at least some
// diagnostics are not present without it.
// RUN: not %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -parse-as-library -module-name Test -target %target-next-stable-abi-triple 2>&1 | %FileCheck --check-prefix NON_MIN %s


// Check that -target-min-inlining-version overrides -library-level, allowing
// library owners to disable this behavior for API libraries if needed.
// RUN: not %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -parse-as-library -module-name Test -target %target-next-stable-abi-triple -target-min-inlining-version target -library-level api -require-explicit-availability=ignore 2>&1 | %FileCheck --check-prefix NON_MIN %s


// Check that we respect -target-min-inlining-version by cranking it up high
// enough to suppress any possible errors.
// RUN: %target-swift-frontend -typecheck -disable-objc-attr-requires-foundation-module %s -swift-version 5 -enable-library-evolution  -parse-as-library -module-name Test -target %target-next-stable-abi-triple -target-min-inlining-version 42.0


// NON_MIN: error: expected error not produced
// NON_MIN: {'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}


// MARK: - Struct definitions

/// Declaration with no availability annotation. Should be inferred as minimum
/// inlining target.
public struct NoAvailable {
  @usableFromInline internal init() {}
}

@available(macOS 10.9, *)
public struct BeforeInliningTarget {
  @usableFromInline internal init() {}
}

@available(macOS 10.10, *)
public struct AtInliningTarget {
  @usableFromInline internal init() {}
}

@available(macOS 10.14.5, *)
public struct BetweenTargets { // expected-note {{enclosing scope requires availability of macOS 10.14.5 or newer}}
  @usableFromInline internal init() {}
}

@available(macOS 10.15, *)
public struct AtDeploymentTarget {
  @usableFromInline internal init() {}
}

@available(macOS 11, *)
public struct AfterDeploymentTarget {
  @usableFromInline internal init() {}
}

@available(macOS, unavailable)
public struct Unavailable {
  @usableFromInline internal init() {}
}

// MARK: - Protocol definitions

public protocol NoAvailableProto {}

@available(macOS 10.9, *)
public protocol BeforeInliningTargetProto {}

@available(macOS 10.10, *)
public protocol AtInliningTargetProto {}

@available(macOS 10.14.5, *)
public protocol BetweenTargetsProto {}

@available(macOS 10.15, *)
public protocol AtDeploymentTargetProto {}

@available(macOS 11, *)
public protocol AfterDeploymentTargetProto {}

@available(macOS, unavailable)
public protocol UnavailableProto {}

// MARK: - Class definitions

public class NoAvailableClass {}

@available(macOS 10.9, *)
public class BeforeInliningTargetClass {}

@available(macOS 10.10, *)
public class AtInliningTargetClass {}

@available(macOS 10.14.5, *)
public class BetweenTargetsClass {}

@available(macOS 10.15, *)
public class AtDeploymentTargetClass {}

@available(macOS 11, *)
public class AfterDeploymentTargetClass {}

@available(macOS, unavailable)
public class UnavailableClass {}


// MARK: - Internal functions

//
// Both the signature and the body of internal functions should be typechecked
// using the minimum deployment target.
//

internal func internalFn( // expected-note 3 {{add @available attribute to enclosing global function}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

// MARK: - Resilient functions

//
// The body of a resilient function is typechecked using the minimum deployment
// but the function's signature should be checked with the inlining target.
//

public func deployedUseNoAvailable( // expected-note 5 {{add @available attribute}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.9, *)
public func deployedUseBeforeInliningTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.10, *)
public func deployedUseAtInliningTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.14.5, *)
public func deployedUseBetweenTargets(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.15, *)
public func deployedUseAtDeploymentTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 11, *)
public func deployedUseAfterDeploymentTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget()
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget()
}

@available(macOS, unavailable)
public func alwaysUnavailable(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget,
  _: Unavailable
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  _ = Unavailable()
  
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@_spi(Private)
public func spiDeployedUseNoAvailable( // expected-note 3 {{add @available attribute}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

// MARK: - @inlinable functions

//
// Both the bodies and signatures of inlinable functions need to be typechecked
// using the minimum inlining target.
//

@inlinable public func inlinedUseNoAvailable( // expected-note 8 {{add @available attribute}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.14.5, *) {
    _ = BetweenTargets()
  }
  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.9, *)
@inlinable public func inlinedUseBeforeInliningTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.14.5, *) {
    _ = BetweenTargets()
  }
  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.10, *)
@inlinable public func inlinedUseAtInliningTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.14.5, *) {
    _ = BetweenTargets()
  }
  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.14.5, *)
@inlinable public func inlinedUseBetweenTargets(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 10.15, *)
@inlinable public func inlinedUseAtDeploymentTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@available(macOS 11, *)
@inlinable public func inlinedUseAfterDeploymentTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget()
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget()
}

@available(macOS, unavailable)
@inlinable public func inlinedAlwaysUnavailable(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget,
  _: Unavailable
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  _ = Unavailable()

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

@_spi(Private)
@inlinable public func spiInlinedUseNoAvailable( // expected-note 3 {{add @available attribute}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets,
  _: AtDeploymentTarget,
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}

// MARK: - @inlinable global property accessors

@inlinable public var inlinedNoAvailableGlobal: Any { // expected-note 3 {{add @available attribute to enclosing var}}
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }

  return ()
}

@inlinable public var inlinedNoAvailableGlobalExplicitGetter: Any { // expected-note 3 {{add @available attribute to enclosing var}}
  get {
    _ = NoAvailable()
    _ = BeforeInliningTarget()
    _ = AtInliningTarget()
    _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget()
    }

    return ()
  }
}

@available(macOS 10.15, *)
@inlinable public var inlinedAtDeploymentTargetGlobal: Any {
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }

  return ()
}

@inlinable public var inlinedGlobalGetterAtDeploymentTarget: Any { // expected-note {{add @available attribute to enclosing var}}
  @available(macOS 10.15, *)
  get {
    _ = NoAvailable()
    _ = BeforeInliningTarget()
    _ = AtInliningTarget()
    _ = BetweenTargets()
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget()
    }

    return ()
  }
}

@_spi(Private)
@inlinable public var inlinedNoAvailableSPIGlobal: Any { // expected-note {{add @available attribute to enclosing var}}
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }

  return ()
}

@inlinable public var inlinedNoAvailableGlobalSPISetter: Any { // expected-note {{add @available attribute to enclosing var}}
  get {
    fatalError()
  }
  @_spi(Private)
  set {
    _ = NoAvailable()
    _ = BeforeInliningTarget()
    _ = AtInliningTarget()
    _ = BetweenTargets()
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget()
    }
  }
}

@available(macOS, unavailable)
@inlinable public var inlinedUnavailableGlobal: Any {
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets()
  _ = AtDeploymentTarget()
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }

  return ()
}

@inlinable public var inlinedNoAvailableGlobalUnavailableSetter: Any { // expected-note {{add @available attribute to enclosing var}}
  get {
    fatalError()
  }
  @available(macOS, unavailable)
  set {
    _ = NoAvailable()
    _ = BeforeInliningTarget()
    _ = AtInliningTarget()
    _ = BetweenTargets()
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget()
    }
  }
}

// MARK: - @_alwaysEmitIntoClient functions

// @_alwaysEmitIntoClient acts like @inlinable.

@_alwaysEmitIntoClient public func aEICUseNoAvailable( // expected-note 8 {{add @available attribute}}
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.14.5, *) {
    _ = BetweenTargets()
  }
  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}


// MARK: - @backDeployed functions

// @backDeployed acts like @inlinable.

@available(macOS 10.10, *)
@backDeployed(before: macOS 999.0)
public func backDeployedToInliningTarget(
  _: NoAvailable,
  _: BeforeInliningTarget,
  _: AtInliningTarget,
  _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {
  defer {
    _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
  }
  _ = NoAvailable()
  _ = BeforeInliningTarget()
  _ = AtInliningTarget()
  _ = BetweenTargets() // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
  _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

  if #available(macOS 10.14.5, *) {
    _ = BetweenTargets()
  }
  if #available(macOS 10.15, *) {
    _ = AtDeploymentTarget()
  }
  if #available(macOS 11, *) {
    _ = AfterDeploymentTarget()
  }
}


// MARK: - Default arguments

// Default arguments act like @inlinable when in a public function.

public func defaultArgsUseNoAvailable( // expected-note 3 {{add @available attribute}}
  _: Any = NoAvailable.self,
  _: Any = BeforeInliningTarget.self,
  _: Any = AtInliningTarget.self,
  _: Any = BetweenTargets.self, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  _: Any = AtDeploymentTarget.self, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  _: Any = AfterDeploymentTarget.self // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {}

func defaultArgsUseInternal( // expected-note {{add @available attribute}}
  _: Any = NoAvailable.self,
  _: Any = BeforeInliningTarget.self,
  _: Any = AtInliningTarget.self,
  _: Any = BetweenTargets.self,
  _: Any = AtDeploymentTarget.self,
  _: Any = AfterDeploymentTarget.self // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {}

@available(macOS, unavailable)
public func defaultArgsUseUnavailable(
  _: Any = NoAvailable.self,
  _: Any = BeforeInliningTarget.self,
  _: Any = AtInliningTarget.self,
  _: Any = BetweenTargets.self,
  _: Any = AtDeploymentTarget.self,
  _: Any = AfterDeploymentTarget.self, // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  _: Any = Unavailable.self
) {}

@_spi(Private)
public func spiDefaultArgsUseNoAvailable( // expected-note 1 {{add @available attribute}}
  _: Any = NoAvailable.self,
  _: Any = BeforeInliningTarget.self,
  _: Any = AtInliningTarget.self,
  _: Any = BetweenTargets.self,
  _: Any = AtDeploymentTarget.self,
  _: Any = AfterDeploymentTarget.self // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
) {}

// Verify that complex default argument expressions are checked appropriately.
public func defaultArgsClosureExprNoAvailable( // expected-note 3 {{add @available attribute}}
  _: Int = {
    _ = NoAvailable.self
    _ = BeforeInliningTarget.self
    _ = AtInliningTarget.self
    _ = BetweenTargets.self // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
    _ = AtDeploymentTarget.self // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available' version check}}
    _ = AfterDeploymentTarget.self // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available' version check}}
    if #available(macOS 10.14.5, *) {
      _ = BetweenTargets.self
    }
    if #available(macOS 10.15, *) {
      _ = AtDeploymentTarget.self
    }
    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget.self
    }
    return 42
  }()
) {}

func defaultArgsClosureExprInternal( // expected-note {{add @available attribute}}
  _: Int = {
    _ = NoAvailable.self
    _ = BeforeInliningTarget.self
    _ = AtInliningTarget.self
    _ = BetweenTargets.self
    _ = AtDeploymentTarget.self
    _ = AfterDeploymentTarget.self // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available' version check}}
    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget.self
    }
    return 42
  }()
) {}


// MARK: - Properties

@propertyWrapper
public struct PropertyWrapper<T> {
  public var wrappedValue: T

  public init(wrappedValue value: T) { self.wrappedValue = value }
  public init(_ value: T) { self.wrappedValue = value }
}

public struct PublicStruct { // expected-note 15 {{add @available attribute}}
  // Public property declarations are exposed.
  public var aPublic: NoAvailable,
             bPublic: BeforeInliningTarget,
             cPublic: AtInliningTarget,
             dPublic: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
             ePublic: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
             fPublic: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS 10.14.5, *)
  public var aPublicAvailBetween: NoAvailable,
             bPublicAvailBetween: BeforeInliningTarget,
             cPublicAvailBetween: AtInliningTarget,
             dPublicAvailBetween: BetweenTargets,
             ePublicAvailBetween: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
             fPublicAvailBetween: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @_spi(Private)
  public var aSPI: NoAvailable,
             bSPI: BeforeInliningTarget,
             cSPI: AtInliningTarget,
             dSPI: BetweenTargets,
             eSPI: AtDeploymentTarget,
             fSPI: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS, unavailable)
  public var aUnavailable: NoAvailable {
    NoAvailable()
  }

  @available(macOS, unavailable)
  public var bUnavailable: BeforeInliningTarget {
    BeforeInliningTarget()
  }

  @available(macOS, unavailable)
  public var cUnavailable: AtInliningTarget {
    AtInliningTarget()
  }

  @available(macOS, unavailable)
  public var dUnavailable: BetweenTargets {
    BetweenTargets()
  }

  @available(macOS, unavailable)
  public var eUnavailable: AtDeploymentTarget {
    AtDeploymentTarget()
  }

  @available(macOS, unavailable)
  public var fUnavailable: AfterDeploymentTarget {
    AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available' version check}}
  }

  // The inferred types of public properties are exposed.
  public var aPublicInferred = NoAvailable(),
             bPublicInferred = BeforeInliningTarget(),
             cPublicInferred = AtInliningTarget(),
             dPublicInferred = BetweenTargets(), // FIXME: Inferred type should be diagnosed
             ePublicInferred = AtDeploymentTarget(), // FIXME: Inferred type should be diagnosed
             fPublicInferred = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS 10.14.5, *)
  public var aPublicInferredAvailBetween = NoAvailable(),
             bPublicInferredAvailBetween = BeforeInliningTarget(),
             cPublicInferredAvailBetween = AtInliningTarget(),
             dPublicInferredAvailBetween = BetweenTargets(),
             ePublicInferredAvailBetween = AtDeploymentTarget(), // FIXME: Inferred type should be diagnosed
             fPublicInferredAvailBetween = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  
  // Property initializers are not exposed.
  public var aPublicInit: Any = NoAvailable(),
             bPublicInit: Any = BeforeInliningTarget(),
             cPublicInit: Any = AtInliningTarget(),
             dPublicInit: Any = BetweenTargets(),
             ePublicInit: Any = AtDeploymentTarget(),
             fPublicInit: Any = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  
  // Internal declarations are not exposed.
  var aInternal: NoAvailable = .init(),
      bInternal: BeforeInliningTarget = .init(),
      cInternal: AtInliningTarget = .init(),
      dInternal: BetweenTargets = .init(),
      eInternal: AtDeploymentTarget = .init(),
      fInternal: AfterDeploymentTarget = .init() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS 10.14.5, *)
  public internal(set) var internalSetter: Void {
    @inlinable get {
      // Public inlinable getter acts like @inlinable
      _ = NoAvailable()
      _ = BeforeInliningTarget()
      _ = AtInliningTarget()
      _ = BetweenTargets()
      _ = AtDeploymentTarget() // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add 'if #available'}}
      _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

      if #available(macOS 10.15, *) {
        _ = AtDeploymentTarget()
      }
      if #available(macOS 11, *) {
        _ = AfterDeploymentTarget()
      }
    }
    set {
      // Private setter acts like non-inlinable
      _ = NoAvailable()
      _ = BeforeInliningTarget()
      _ = AtInliningTarget()
      _ = BetweenTargets()
      _ = AtDeploymentTarget()
      _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}

      if #available(macOS 11, *) {
        _ = AfterDeploymentTarget()
      }
    }
  }
  
  public var block: () -> () = {
    // The body of a block assigned to a public property acts like non-@inlinable
    _ = NoAvailable()
    _ = BeforeInliningTarget()
    _ = AtInliningTarget()
    _ = BetweenTargets()
    _ = AtDeploymentTarget()
    _ = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add 'if #available'}}
    
    if #available(macOS 11, *) {
      _ = AfterDeploymentTarget()
    }
  }
}

public struct PublicStructWithWrappers { // expected-note 4 {{add @available attribute}}
  // The property type is inferred from the initializer expression. The
  // expressions themselves will not be exposed.
  @PropertyWrapper public var aExplicitInit = NoAvailable()
  @PropertyWrapper public var bExplicitInit = BeforeInliningTarget()
  @PropertyWrapper public var cExplicitInit = AtInliningTarget()
  @PropertyWrapper public var dExplicitInit = BetweenTargets() // FIXME: Inferred type should be diagnosed
  @PropertyWrapper public var eExplicitInit = AtDeploymentTarget() // FIXME: Inferred type should be diagnosed
  @PropertyWrapper public var fExplicitInit = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  
  // The property type is inferred from the initializer expression. The
  // expressions themselves will not be exposed.
  @PropertyWrapper(NoAvailable()) public var aExplicitInitAlt
  @PropertyWrapper(BeforeInliningTarget()) public var bExplicitInitAlt
  @PropertyWrapper(AtInliningTarget()) public var cExplicitInitAlt
  @PropertyWrapper(BetweenTargets()) public var dExplicitInitAlt  // FIXME: Inferred type should be diagnosed
  @PropertyWrapper(AtDeploymentTarget()) public var ePExplicitInitAlt  // FIXME: Inferred type should be diagnosed
  @PropertyWrapper(AfterDeploymentTarget()) public var fExplicitInitAlt // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  // The property type is explicitly `Any` and the initializer expressions are
  // not exposed.
  @PropertyWrapper public var aAny: Any = NoAvailable()
  @PropertyWrapper public var bAny: Any = BeforeInliningTarget()
  @PropertyWrapper public var cAny: Any = AtInliningTarget()
  @PropertyWrapper public var dAny: Any = BetweenTargets()
  @PropertyWrapper public var eAny: Any = AtDeploymentTarget()
  @PropertyWrapper public var fAny: Any = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  // The property type is explicitly `Any` and the initializer expressions are
  // not exposed.
  @PropertyWrapper(NoAvailable()) public var aAnyAlt: Any
  @PropertyWrapper(BeforeInliningTarget()) public var bAnyAlt: Any
  @PropertyWrapper(AtInliningTarget()) public var cAnyAlt: Any
  @PropertyWrapper(BetweenTargets()) public var dAnyAlt: Any
  @PropertyWrapper(AtDeploymentTarget()) public var eAnyAlt: Any
  @PropertyWrapper(AfterDeploymentTarget()) public var fAnyAlt: Any // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
}

@frozen public struct FrozenPublicStruct { // expected-note 9 {{add @available attribute}}
  // Public declarations are exposed.
  public var aPublic: NoAvailable,
             bPublic: BeforeInliningTarget,
             cPublic: AtInliningTarget,
             dPublic: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
             ePublic: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
             fPublic: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  // Property initializers are exposed in frozen structs.
  public var aPublicInit: Any = NoAvailable(),
             bPublicInit: Any = BeforeInliningTarget(),
             cPublicInit: Any = AtInliningTarget(),
             dPublicInit: Any = BetweenTargets(), // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
             ePublicInit: Any = AtDeploymentTarget(), // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
             fPublicInit: Any = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  // Internal declarations are also exposed in frozen structs.
  var aInternal: NoAvailable = .init(),
      bInternal: BeforeInliningTarget = .init(),
      cInternal: AtInliningTarget = .init(),
      dInternal: BetweenTargets = .init(), // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
      eInternal: AtDeploymentTarget = .init(), // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
      fInternal: AfterDeploymentTarget = .init() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
}

@available(macOS, unavailable)
public struct UnavailablePublicStruct {
  public var aPublic: NoAvailable,
             bPublic: BeforeInliningTarget,
             cPublic: AtInliningTarget,
             dPublic: BetweenTargets,
             ePublic: AtDeploymentTarget,
             fPublic: AfterDeploymentTarget, // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
             gPublic: Unavailable

  public var aPublicInit: Any = NoAvailable(),
             bPublicInit: Any = BeforeInliningTarget(),
             cPublicInit: Any = AtInliningTarget(),
             dPublicInit: Any = BetweenTargets(),
             ePublicInit: Any = AtDeploymentTarget(),
             fPublicInit: Any = AfterDeploymentTarget(), // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
             gPublicInit: Any = Unavailable()

  var aInternal: NoAvailable = .init(),
      bInternal: BeforeInliningTarget = .init(),
      cInternal: AtInliningTarget = .init(),
      dInternal: BetweenTargets = .init(),
      eInternal: AtDeploymentTarget = .init(),
      fInternal: AfterDeploymentTarget = .init(), // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
      gInternal: Unavailable = .init()
}

@_spi(Private)
public struct SPIStruct { // expected-note 3 {{add @available attribute}}
  public var aPublic: NoAvailable,
             bPublic: BeforeInliningTarget,
             cPublic: AtInliningTarget,
             dPublic: BetweenTargets,
             ePublic: AtDeploymentTarget,
             fPublic: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  public var aPublicInit: Any = NoAvailable(),
             bPublicInit: Any = BeforeInliningTarget(),
             cPublicInit: Any = AtInliningTarget(),
             dPublicInit: Any = BetweenTargets(),
             ePublicInit: Any = AtDeploymentTarget(),
             fPublicInit: Any = AfterDeploymentTarget() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  var aInternal: NoAvailable = .init(),
      bInternal: BeforeInliningTarget = .init(),
      cInternal: AtInliningTarget = .init(),
      dInternal: BetweenTargets = .init(),
      eInternal: AtDeploymentTarget = .init(),
      fInternal: AfterDeploymentTarget = .init() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
}

internal struct InternalStruct { // expected-note 2 {{add @available attribute}}
  // Internal declarations act like non-inlinable.
  var aInternal: NoAvailable = .init(),
      bInternal: BeforeInliningTarget = .init(),
      cInternal: AtInliningTarget = .init(),
      dInternal: BetweenTargets = .init(),
      eInternal: AtDeploymentTarget = .init(),
      fInternal: AfterDeploymentTarget = .init() // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @PropertyWrapper(NoAvailable()) var aWrapped: Any
  @PropertyWrapper(BeforeInliningTarget()) var bWrapped: Any
  @PropertyWrapper(AtInliningTarget()) var cWrapped: Any
  @PropertyWrapper(BetweenTargets()) var dWrapped: Any
  @PropertyWrapper(AtDeploymentTarget()) var eWrapped: Any
  @PropertyWrapper(AfterDeploymentTarget()) var fWrapped: Any // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
}


// MARK: - Extensions

//
// Extensions are externally visible if they extend a public type and (1) have
// public members or (2) declare a conformance to a public protocol.
//
// Extensions without explicit availability that are externally visible use an
// implied floor of either the availability of the extended type or the
// deployment target, whichever is more available. This is a special rule
// designed as a convenience to library authors who have written quite a bit of
// code without annotating availability on their extensions and getting away
// with it because previously the deployment target was always used as the
// floor.
//
// Extensions without explicit availability that are not visible externally are
// checked using an implied floor of the deployment target.
//


// MARK: Extensions on NoAvailable

extension NoAvailable {}

extension NoAvailable { // expected-note {{add @available attribute to enclosing extension}}
  func internalFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

extension NoAvailable { // expected-note 3 {{add @available attribute to enclosing extension}}
  public func publicFuncInExtension( // expected-note 3 {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets, // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
    _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

// MARK: Extensions on BetweenTargets

extension BetweenTargets {}

extension BetweenTargets { // expected-note {{add @available attribute to enclosing extension}}
  func internalFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

extension BetweenTargets { // expected-note 2 {{add @available attribute to enclosing extension}}
  public func publicFuncInExtension( // expected-note 2 {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

@available(macOS 10.15, *)
extension BetweenTargets {
  public func publicFuncInExtensionWithExplicitAvailability( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

extension BetweenTargets { // expected-note {{add @available attribute to enclosing extension}}
  @available(macOS 10.15, *)
  public func publicFuncWithExplicitAvailabilityInExtension(
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

@_spi(Private)
extension BetweenTargets { // expected-note {{add @available attribute to enclosing extension}}
  public func inheritedSPIFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

@available(macOS, unavailable)
extension BetweenTargets {
  public func inheritsUnavailableFuncInExtension(
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget,
    _: Unavailable
  ) {}
}

// This extension is explicitly more available than BetweenTargets but because
// it only contains internal members, the availability floor is still the
// deployment target.
@available(macOS 10.10, *)
extension BetweenTargets {
  func internalFuncInExcessivelyAvailableExtension() {}
}

extension BetweenTargets {
  @available(macOS 10.10, *) // expected-warning {{instance method cannot be more available than enclosing scope}}
  func excessivelyAvailableInternalFuncInExtension() {}
}

@available(macOS 10.10, *)
extension BetweenTargets { // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  public func publicFuncInExcessivelyAvailableExtension() {}
}

// MARK: Extensions on BetweenTargetsInternal

// Same availability as BetweenTargets but internal instead of public.
@available(macOS 10.14.5, *)
internal struct BetweenTargetsInternal {}

extension BetweenTargetsInternal {}

extension BetweenTargetsInternal { // expected-note {{add @available attribute to enclosing extension}}
  func internalFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

extension BetweenTargetsInternal { // expected-note {{add @available attribute to enclosing extension}}
  public func publicFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

// MARK: Extensions on AfterDeploymentTarget

// expected-error@+1 {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note@+1 {{add @available attribute to enclosing extension}}
extension AfterDeploymentTarget {}

// expected-error@+1 {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
extension AfterDeploymentTarget { // expected-note 2 {{add @available attribute to enclosing extension}}
  func internalFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

// expected-error@+1 {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
extension AfterDeploymentTarget { // expected-note 2 {{add @available attribute to enclosing extension}}
  public func publicFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

@available(macOS 11, *)
extension AfterDeploymentTarget {
  public func publicFuncInExtensionWithExplicitAvailability(
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget
  ) {}
}

// MARK: Extensions on nested types

@available(macOS 10.14.5, *)
public enum BetweenTargetsEnum {
  public struct Nested {}
}

extension BetweenTargetsEnum.Nested {}

extension BetweenTargetsEnum.Nested { // expected-note {{add @available attribute to enclosing extension}}
  func internalFuncInExtension( // expected-note {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget,
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

extension BetweenTargetsEnum.Nested { // expected-note 2 {{add @available attribute to enclosing extension}}
  public func publicFuncInExtension( // expected-note 2 {{add @available attribute to enclosing instance method}}
    _: NoAvailable,
    _: BeforeInliningTarget,
    _: AtInliningTarget,
    _: BetweenTargets,
    _: AtDeploymentTarget, // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
    _: AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}
  ) {}
}

// MARK: Protocol conformances

internal protocol InternalProto {}

extension NoAvailable: InternalProto {}
extension BeforeInliningTarget: InternalProto {}
extension AtInliningTarget: InternalProto {}
extension BetweenTargets: InternalProto {}
extension AtDeploymentTarget: InternalProto {}
extension AfterDeploymentTarget: InternalProto {} // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing extension}}

public protocol PublicProto {}

extension NoAvailable: PublicProto {}
extension BeforeInliningTarget: PublicProto {}
extension AtInliningTarget: PublicProto {}
extension BetweenTargets: PublicProto {}
extension AtDeploymentTarget: PublicProto {}
extension AfterDeploymentTarget: PublicProto {} // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing extension}}


// MARK: - Associated types

public protocol NoAvailableProtoWithAssoc { // expected-note 3 {{add @available attribute to enclosing protocol}}
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto // expected-error {{'BetweenTargetsProto' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype E: AtDeploymentTargetProto // expected-error {{'AtDeploymentTargetProto' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

@available(macOS 10.9, *)
public protocol BeforeInliningTargetProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto // expected-error {{'BetweenTargetsProto' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype E: AtDeploymentTargetProto // expected-error {{'AtDeploymentTargetProto' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

@available(macOS 10.10, *)
public protocol AtInliningTargetProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto // expected-error {{'BetweenTargetsProto' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype E: AtDeploymentTargetProto // expected-error {{'AtDeploymentTargetProto' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

@available(macOS 10.14.5, *)
public protocol BetweenTargetsProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto
  associatedtype E: AtDeploymentTargetProto // expected-error {{'AtDeploymentTargetProto' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

@available(macOS 10.15, *)
public protocol AtDeploymentTargetProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto
  associatedtype E: AtDeploymentTargetProto
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

@available(macOS 11, *)
public protocol AfterDeploymentTargetProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto
  associatedtype E: AtDeploymentTargetProto
  associatedtype F: AfterDeploymentTargetProto
}

@available(macOS, unavailable)
public protocol UnavailableProtoWithAssoc {
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto
  associatedtype E: AtDeploymentTargetProto
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
  associatedtype G: UnavailableProto
}

@_spi(Private)
public protocol SPINoAvailableProtoWithAssoc { // expected-note 1 {{add @available attribute to enclosing protocol}}
  associatedtype A: NoAvailableProto
  associatedtype B: BeforeInliningTargetProto
  associatedtype C: AtInliningTargetProto
  associatedtype D: BetweenTargetsProto
  associatedtype E: AtDeploymentTargetProto
  associatedtype F: AfterDeploymentTargetProto // expected-error {{'AfterDeploymentTargetProto' is only available in}}
}

// MARK: - Type aliases

public enum PublicNoAvailableEnumWithTypeAliases { // expected-note 3 {{add @available attribute to enclosing enum}}
  public typealias A = NoAvailable
  public typealias B = BeforeInliningTarget
  public typealias C = AtInliningTarget
  public typealias D = BetweenTargets // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add @available attribute to enclosing type alias}}
  public typealias E = AtDeploymentTarget // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note {{add @available attribute to enclosing type alias}}
  public typealias F = AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing type alias}}
}

@available(macOS, unavailable)
public enum UnavailableEnumWithTypeAliases {
  public typealias A = NoAvailable
  public typealias B = BeforeInliningTarget
  public typealias C = AtInliningTarget
  public typealias D = BetweenTargets
  public typealias E = AtDeploymentTarget
  public typealias F = AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing type alias}}
  public typealias G = Unavailable
}

@_spi(Private)
public enum SPIEnumWithTypeAliases { // expected-note 1 {{add @available attribute to enclosing enum}}
  public typealias A = NoAvailable
  public typealias B = BeforeInliningTarget
  public typealias C = AtInliningTarget
  public typealias D = BetweenTargets
  public typealias E = AtDeploymentTarget
  public typealias F = AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing type alias}}
}

enum InternalNoAvailableEnumWithTypeAliases { // expected-note {{add @available attribute to enclosing enum}}
  public typealias A = NoAvailable
  public typealias B = BeforeInliningTarget
  public typealias C = AtInliningTarget
  public typealias D = BetweenTargets
  public typealias E = AtDeploymentTarget
  public typealias F = AfterDeploymentTarget // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}} expected-note {{add @available attribute to enclosing type alias}}
}

// MARK: - Enums with payloads

public enum PublicNoAvailableEnumWithPayloads { // expected-note 5 {{add @available attribute to enclosing enum}}
  case aNoAvailable(NoAvailable),
       bNoAvailable(BeforeInliningTarget),
       cNoAvailable(AtInliningTarget),
       dNoAvailable(BetweenTargets), // expected-error {{'BetweenTargets' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}}
       eNoAvailable(AtDeploymentTarget), // expected-error {{'AtDeploymentTarget' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}}
       fNoAvailable(AfterDeploymentTarget) // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS, introduced: 10.15)
  case aAtDeploymentTarget(NoAvailable),
       bAtDeploymentTarget(BeforeInliningTarget),
       cAtDeploymentTarget(AtInliningTarget),
       dAtDeploymentTarget(BetweenTargets),
       eAtDeploymentTarget(AtDeploymentTarget),
       fAtDeploymentTarget(AfterDeploymentTarget) // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @_spi(Private)
  case aSPI(NoAvailable),
       bSPI(BeforeInliningTarget),
       cSPI(AtInliningTarget),
       dSPI(BetweenTargets),
       eSPI(AtDeploymentTarget),
       fSPI(AfterDeploymentTarget) // expected-error {{'AfterDeploymentTarget' is only available in macOS 11 or newer}}

  @available(macOS, unavailable)
  case aUnavailable(NoAvailable),
       bUnavailable(BeforeInliningTarget),
       cUnavailable(AtInliningTarget),
       dUnavailable(BetweenTargets),
       eUnavailable(AtDeploymentTarget),
       fUnavailable(AfterDeploymentTarget)
}

// MARK: - Class inheritance

// FIXME: Duplicate 'add @available' emitted when classes are nested in a decl

public enum NoAvailableEnumWithClasses {
  public class InheritsNoAvailable: NoAvailableClass {}
  public class InheritsBeforeInliningTarget: BeforeInliningTargetClass {}
  public class InheritsAtInliningTarget: AtInliningTargetClass {}
  public class InheritsBetweenTargets: BetweenTargetsClass {} // expected-error {{'BetweenTargetsClass' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note 2 {{add @available attribute to enclosing class}}
  public class InheritsAtDeploymentTarget: AtDeploymentTargetClass {} // expected-error {{'AtDeploymentTargetClass' is only available in macOS 10.15 or newer; clients of 'Test' may have a lower deployment target}} expected-note 2 {{add @available attribute to enclosing class}}
  public class InheritsAfterDeploymentTarget: AfterDeploymentTargetClass {} // expected-error {{'AfterDeploymentTargetClass' is only available in macOS 11 or newer}} expected-note 2 {{add @available attribute to enclosing class}}
  
  @usableFromInline
  class UFIInheritsBetweenTargets: BetweenTargetsClass {} // expected-error {{'BetweenTargetsClass' is only available in macOS 10.14.5 or newer; clients of 'Test' may have a lower deployment target}} expected-note 2 {{add @available attribute to enclosing class}}
}

@_spi(Private)
public enum SPIEnumWithClasses {
  public class InheritsNoAvailable: NoAvailableClass {}
  public class InheritsBeforeInliningTarget: BeforeInliningTargetClass {}
  public class InheritsAtInliningTarget: AtInliningTargetClass {}
  public class InheritsBetweenTargets: BetweenTargetsClass {}
  public class InheritsAtDeploymentTarget: AtDeploymentTargetClass {}
  // FIXME: Duplicate 'add @available' note is emitted
  public class InheritsAfterDeploymentTarget: AfterDeploymentTargetClass {} // expected-error {{'AfterDeploymentTargetClass' is only available in}} expected-note 2 {{add @available attribute to enclosing class}}
}

@available(macOS, unavailable)
public enum UnavailableEnumWithClasses {
  public class InheritsNoAvailable: NoAvailableClass {}
  public class InheritsBeforeInliningTarget: BeforeInliningTargetClass {}
  public class InheritsAtInliningTarget: AtInliningTargetClass {}
  public class InheritsBetweenTargets: BetweenTargetsClass {}
  public class InheritsAtDeploymentTarget: AtDeploymentTargetClass {}
  public class InheritsAfterDeploymentTarget: AfterDeploymentTargetClass {} // expected-error {{'AfterDeploymentTargetClass' is only available in}} expected-note 2 {{add @available attribute to enclosing class}}
  public class InheritsUnavailable: UnavailableClass {}
}
