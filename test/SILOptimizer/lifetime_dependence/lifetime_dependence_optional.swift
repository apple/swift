// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

// Simply test that it is possible for a module to define a pseudo-Optional type without triggering any compiler errors.

public protocol ExpressibleByNilLiteral: ~Copyable & ~Escapable {
  // TODO: dependsOn(immortal)
  @_unsafeNonescapableResult
  init(nilLiteral: ())
}

@frozen
public enum Nillable<Wrapped: ~Copyable & ~Escapable>: ~Copyable & ~Escapable {
  case none
  case some(Wrapped)
}

extension Nillable: Copyable where Wrapped: Copyable {}

extension Nillable: Escapable where Wrapped: Escapable {}

extension Nillable: Sendable where Wrapped: ~Copyable & ~Escapable & Sendable { }

extension Nillable: BitwiseCopyable where Wrapped: BitwiseCopyable { }

extension Nillable: ExpressibleByNilLiteral where Wrapped: ~Copyable & ~Escapable {
  // TODO: dependsOn(immortal)
  @_transparent
  @_unsafeNonescapableResult
  public init(nilLiteral: ()) {
    self = .none
  }
}

extension Nillable where Wrapped: ~Copyable & ~Escapable {
  @_transparent
  public init(_ some: consuming Wrapped) { self = .some(some) }
}

extension Nillable where Wrapped: ~Copyable {
  public consuming func _consumingMap<U: ~Copyable, E: Error>(
    _ transform: (consuming Wrapped) throws(E) -> U
  ) throws(E) -> U? {
    switch consume self {
    case .some(let y):
      return .some(try transform(y))
    case .none:
      return .none
    }
  }

  public borrowing func _borrowingMap<U: ~Copyable, E: Error>(
    _ transform: (borrowing Wrapped) throws(E) -> U
  ) throws(E) -> U? {
    switch self {
    case .some(let y):
      return .some(try transform(y))
    case .none:
      return .none
    }
  }
}

extension Nillable where Wrapped: ~Copyable {
  public consuming func _consumingFlatMap<U: ~Copyable, E: Error>(
    _ transform: (consuming Wrapped) throws(E) -> U?
  ) throws(E) -> U? {
    switch consume self {
    case .some(let y):
      return try transform(consume y)
    case .none:
      return .none
    }
  }

  public func _borrowingFlatMap<U: ~Copyable, E: Error>(
    _ transform: (borrowing Wrapped) throws(E) -> U?
  ) throws(E) -> U? {
    switch self {
    case .some(let y):
      return try transform(y)
    case .none:
      return .none
    }
  }
}

extension Nillable where Wrapped: ~Copyable {
  public consuming func _consumingUnsafelyUnwrap() -> Wrapped {
    switch consume self {
    case .some(let x):
      return x
    case .none:
      fatalError("consumingUsafelyUnwrap of nil optional")
    }
  }
}

extension Optional where Wrapped: ~Copyable {
  internal consuming func _consumingUncheckedUnwrapped() -> Wrapped {
    if let x = self {
      return x
    }
    fatalError("_uncheckedUnwrapped of nil optional")
  }
}

extension Optional where Wrapped: ~Copyable {
  public mutating func _take() -> Self {
    let result = consume self
    self = nil
    return result
  }
}

extension Optional where Wrapped: ~Copyable {
  public static func ~=(
    lhs: _OptionalNilComparisonType,
    rhs: borrowing Wrapped?
  ) -> Bool {
    switch rhs {
    case .some:
      return false
    case .none:
      return true
    }
  }

  public static func ==(
    lhs: borrowing Wrapped?,
    rhs: _OptionalNilComparisonType
  ) -> Bool {
    switch lhs {
    case .some:
      return false
    case .none:
      return true
    }
  }

  public static func !=(
    lhs: borrowing Wrapped?,
    rhs: _OptionalNilComparisonType
  ) -> Bool {
    switch lhs {
    case .some:
      return true
    case .none:
      return false
    }
  }

  public static func ==(
    lhs: _OptionalNilComparisonType,
    rhs: borrowing Wrapped?
  ) -> Bool {
    switch rhs {
    case .some:
      return false
    case .none:
      return true
    }
  }

  public static func !=(
    lhs: _OptionalNilComparisonType,
    rhs: borrowing Wrapped?
  ) -> Bool {
    switch rhs {
    case .some:
      return true
    case .none:
      return false
    }
  }
}

public func ?? <T: ~Copyable>(
  optional: consuming T?,
  defaultValue: @autoclosure () throws -> T
) rethrows -> T {
  switch consume optional {
  case .some(let value):
    return value
  case .none:
    return try defaultValue()
  }
}

public func ?? <T: ~Copyable>(
  optional: consuming T?,
  defaultValue: @autoclosure () throws -> T?
) rethrows -> T? {
  switch consume optional {
  case .some(let value):
    return value
  case .none:
    return try defaultValue()
  }
}
