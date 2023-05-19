// Test that the -require-explicit-availability flag does not cause diagnostics
// to be emitted on platforms where versioned availability annotations are not
// meaningful.

// RUN: %target-swift-frontend -typecheck -parse-as-library -verify %s -require-explicit-availability=error

// Currently versioned availability should only be required on Apple platforms.
// UNSUPPORTED: VENDOR=apple

public struct NoAvailabilityStruct {
  public func method() { }
}

@available(*, unavailable)
public struct UnavailableStruct {
  public func okMethod() { }
}

public func noAvailabilityFunc() { }

@available(SwiftStdlib 5.9, *)
public func stdlib5_9AvailabilityFunc() { }
