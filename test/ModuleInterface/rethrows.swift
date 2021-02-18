// RUN: %target-swift-frontend -typecheck -emit-module-interface-path - %s -enable-library-evolution -module-name MyModule | %FileCheck %s

// CHECK-LABEL: {{^}}public func rethrowsFunction(_: () throws -> ()) rethrows
public func rethrowsFunction(_: () throws -> ()) rethrows {}

// CHECK-LABEL: {{^}}public func uncheckedRethrowsFunction(_: () throws -> ()) rethrows
@_rethrowsUnchecked
public func uncheckedRethrowsFunction(_: () throws -> ()) rethrows {}

// CHECK-LABEL: {{^}}@inlinable @_rethrowsUnchecked public func inlinableUncheckedRethrowsFunction(_: () throws -> ()) rethrows
@inlinable @_rethrowsUnchecked
public func inlinableUncheckedRethrowsFunction(_: () throws -> ()) rethrows {}
