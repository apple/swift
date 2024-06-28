// RUN: %target-typecheck-verify-swift -parse-stdlib -module-name Swift -enable-experimental-feature BuiltinModule -enable-experimental-feature NonescapableTypes



/// This test specifically covers constructs that are only valid in the stdlib.

import Builtin

@_marker public protocol Copyable: ~Escapable {}
@_marker public protocol Escapable: ~Copyable {}

struct NC: ~Copyable {}

@frozen public struct UnsafePointer<T: ~Copyable>: Copyable {
  var value: Builtin.RawPointer
}

@frozen
public enum Optional<T: ~Copyable>: ~Copyable {
  case some(T)
  case none
}

extension Optional: Copyable where T: Copyable {}

public func wrapping<T: ~Copyable>(_ t: consuming T) -> T? {
  return .some(t)
}

// No ownership required.
func checkCopyability(_ t: UnsafePointer<NC>) {}
