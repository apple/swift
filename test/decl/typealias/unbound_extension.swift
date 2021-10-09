// RUN: %target-typecheck-verify-swift

extension UnboundAlias {}

typealias UnboundAlias = GenericType

struct GenericType<T> {}
