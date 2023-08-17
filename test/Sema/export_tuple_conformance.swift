// RUN: %target-typecheck-verify-swift

public struct G<T: Sendable> {}

public func makeG() -> G<Void> {}

// No spurious warning about use of conformance from Builtin module
