//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A type that does not conform to any protocols.
///
/// This type can be used to check that generic functions don't rely on any
/// conformances.
public struct OpaqueValue<Underlying> {
  public var value: Underlying
  public var identity: Int

  public init(_ value: Underlying) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Underlying, identity: Int) {
    self.value = value
    self.identity = identity
  }
}

/// A type that conforms only to `Equatable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public struct MinimalEquatableValue : Equatable {
  public static var timesEqualEqualWasCalled: Int = 0

  public static var equalImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 == $1 })

  public var value: Int
  public var identity: Int

  public init(_ value: Int) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Int, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func == (
    lhs: MinimalEquatableValue,
    rhs: MinimalEquatableValue
  ) -> Bool {
    MinimalEquatableValue.timesEqualEqualWasCalled += 1
    return MinimalEquatableValue.equalImpl.value(lhs.value, rhs.value)
  }
}

/// A type that conforms only to `Equatable` and `Comparable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public struct MinimalComparableValue : Equatable, Comparable {
  public static var timesEqualEqualWasCalled = ResettableValue(0)
  public static var timesLessWasCalled = ResettableValue(0)

  public static var equalImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 == $1 })
  public static var lessImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 < $1 })

  public var value: Int
  public var identity: Int

  public init(_ value: Int) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Int, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func == (
    lhs: MinimalComparableValue,
    rhs: MinimalComparableValue
  ) -> Bool {
    MinimalComparableValue.timesEqualEqualWasCalled.value += 1
    return MinimalComparableValue.equalImpl.value(lhs.value, rhs.value)
  }

  public static func < (
    lhs: MinimalComparableValue,
    rhs: MinimalComparableValue
  ) -> Bool {
    MinimalComparableValue.timesLessWasCalled.value += 1
    return MinimalComparableValue.lessImpl.value(lhs.value, rhs.value)
  }
}


/// A type that conforms only to `Equatable` and `Hashable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public struct MinimalHashableValue : Equatable, Hashable {
  public static var timesEqualEqualWasCalled: Int = 0
  public static var timesHashIntoWasCalled: Int = 0

  public static var equalImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 == $1 })
  public static var hashIntoImpl =
    ResettableValue<(Int, inout Hasher) -> Void>({ $1.combine($0) })

  public var value: Int
  public var identity: Int

  public init(_ value: Int) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Int, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func ==(
    lhs: MinimalHashableValue,
    rhs: MinimalHashableValue
  ) -> Bool {
    MinimalHashableValue.timesEqualEqualWasCalled += 1
    return MinimalHashableValue.equalImpl.value(lhs.value, rhs.value)
  }

  public var hashValue: Int {
    var hasher = Hasher()
    hasher.combine(self)
    return hasher.finalize()
  }

  public func hash(into hasher: inout Hasher) {
    MinimalHashableValue.timesHashIntoWasCalled += 1
    MinimalHashableValue.hashIntoImpl.value(value, &hasher)
  }
}

extension MinimalHashableValue: CustomStringConvertible {
  public var description: String {
    return "MinimalHashableValue(value: \(value), identity: \(identity))"
  }
}


/// A type that conforms only to `Equatable` and `Hashable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public class MinimalHashableClass : Equatable, Hashable {
  public static var timesEqualEqualWasCalled: Int = 0
  public static var timesHashIntoWasCalled: Int = 0

  public static var equalImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 == $1 })
  public static var hashIntoImpl =
  ResettableValue<(Int, inout Hasher) -> Void>({ $1.combine($0) })

  public var value: Int
  public var identity: Int

  public init(_ value: Int) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Int, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func == (
    lhs: MinimalHashableClass,
    rhs: MinimalHashableClass
  ) -> Bool {
    MinimalHashableClass.timesEqualEqualWasCalled += 1
    return MinimalHashableClass.equalImpl.value(lhs.value, rhs.value)
  }

  public var hashValue: Int {
    var hasher = Hasher()
    hasher.combine(self)
    return hasher.finalize()
  }

  public func hash(into hasher: inout Hasher) {
    MinimalHashableClass.timesHashIntoWasCalled += 1
    MinimalHashableClass.hashIntoImpl.value(value, &hasher)
  }
}

extension MinimalHashableClass: CustomStringConvertible {
  public var description: String {
    return "MinimalHashableClass(value: \(value), identity: \(identity))"
  }
}



public var GenericMinimalHashableValue_timesEqualEqualWasCalled: Int = 0
public var GenericMinimalHashableValue_timesHashIntoWasCalled: Int = 0

public var GenericMinimalHashableValue_equalImpl =
  ResettableValue<(Any, Any) -> Bool>({ _, _ in
    fatalError("GenericMinimalHashableValue_equalImpl is not set yet")
  })
public var GenericMinimalHashableValue_hashIntoImpl =
  ResettableValue<(Any, inout Hasher) -> Void>({ (_,_) in
    fatalError("GenericMinimalHashableValue_hashIntoImpl is not set yet")
  })

/// A type that conforms only to `Equatable` and `Hashable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public struct GenericMinimalHashableValue<Wrapped> : Equatable, Hashable {
  public var value: Wrapped
  public var identity: Int

  public init(_ value: Wrapped) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Wrapped, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func ==(
    lhs: GenericMinimalHashableValue<Wrapped>,
    rhs: GenericMinimalHashableValue<Wrapped>
  ) -> Bool {
    GenericMinimalHashableValue_timesEqualEqualWasCalled += 1
    return GenericMinimalHashableValue_equalImpl.value(lhs.value, rhs.value)
  }

  public var hashValue: Int {
    var hasher = Hasher()
    hasher.combine(self)
    return hasher.finalize()
  }

  public func hash(into hasher: inout Hasher) {
    GenericMinimalHashableValue_timesHashIntoWasCalled += 1
    GenericMinimalHashableValue_hashIntoImpl.value(value, &hasher)
  }
}

extension GenericMinimalHashableValue: CustomStringConvertible {
  public var description: String {
    return "GenericMinimalHashableValue(value: \(value), identity: \(identity))"
  }
}

public var GenericMinimalHashableClass_timesEqualEqualWasCalled: Int = 0
public var GenericMinimalHashableClass_timesHashIntoWasCalled: Int = 0

public var GenericMinimalHashableClass_equalImpl =
  ResettableValue<(Any, Any) -> Bool>({ _, _ in
    fatalError("GenericMinimalHashableClass_equalImpl is not set yet")
  })
public var GenericMinimalHashableClass_hashIntoImpl =
  ResettableValue<(Any, inout Hasher) -> Void>({ (_,_) in
    fatalError("GenericMinimalHashableClass_hashIntoImpl is not set yet")
  })

/// A type that conforms only to `Equatable` and `Hashable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public class GenericMinimalHashableClass<Wrapped> : Equatable, Hashable {
  public var value: Wrapped
  public var identity: Int

  public init(_ value: Wrapped) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Wrapped, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public static func ==(
    lhs: GenericMinimalHashableClass<Wrapped>,
    rhs: GenericMinimalHashableClass<Wrapped>
  ) -> Bool {
    GenericMinimalHashableClass_timesEqualEqualWasCalled += 1
    return GenericMinimalHashableClass_equalImpl.value(lhs.value, rhs.value)
  }

  public var hashValue: Int {
    var hasher = Hasher()
    hasher.combine(self)
    return hasher.finalize()
  }

  public func hash(into hasher: inout Hasher) {
    GenericMinimalHashableClass_timesHashIntoWasCalled += 1
    GenericMinimalHashableClass_hashIntoImpl.value(value, &hasher)
  }
}

extension GenericMinimalHashableClass: CustomStringConvertible {
  public var description: String {
    return "GenericMinimalHashableClass(value: \(value), identity: \(identity))"
  }
}


/// A type that conforms only to `Equatable`, `Comparable`, and `Strideable`.
///
/// This type can be used to check that generic functions don't rely on any
/// other conformances.
public struct MinimalStrideableValue : Equatable, Comparable, Strideable {
  public static var timesEqualEqualWasCalled = ResettableValue(0)
  public static var timesLessWasCalled = ResettableValue(0)
  public static var timesDistanceWasCalled = ResettableValue(0)
  public static var timesAdvancedWasCalled = ResettableValue(0)

  public static var equalImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 == $1 })
  public static var lessImpl =
    ResettableValue<(Int, Int) -> Bool>({ $0 < $1 })

  public var value: Int
  public var identity: Int

  public init(_ value: Int) {
    self.value = value
    self.identity = 0
  }

  public init(_ value: Int, identity: Int) {
    self.value = value
    self.identity = identity
  }

  public typealias Stride = Int

  public static func == (
    lhs: MinimalStrideableValue,
    rhs: MinimalStrideableValue
  ) -> Bool {
    MinimalStrideableValue.timesEqualEqualWasCalled.value += 1
    return MinimalStrideableValue.equalImpl.value(lhs.value, rhs.value)
  }

  public static func < (
    lhs: MinimalStrideableValue,
    rhs: MinimalStrideableValue
  ) -> Bool {
    MinimalStrideableValue.timesLessWasCalled.value += 1
    return MinimalStrideableValue.lessImpl.value(lhs.value, rhs.value)
  }

  public func distance(to other: MinimalStrideableValue) -> Stride {
    MinimalStrideableValue.timesDistanceWasCalled.value += 1
    return other.value - self.value
  }

  public func advanced(by n: Stride) -> MinimalStrideableValue {
    MinimalStrideableValue.timesAdvancedWasCalled.value += 1
    return MinimalStrideableValue(self.value + n, identity: self.identity)
  }
}
