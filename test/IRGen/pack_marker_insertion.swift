// RUN: %target-swift-frontend -emit-ir %s

// Verify that we don't hit the `Instruction missing on-stack pack metadata cleanups!` assertion.

// For alloc_stacks of tuples featuring a pack.
public func tupleExpansionWithMemberType<each T: Sequence>(seqs: (repeat each T), elts: (repeat (each T).Element)) {}
