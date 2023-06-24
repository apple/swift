// RUN: %target-swift-frontend -emit-ir -parse-stdlib -module-name=Swift -I%S/Inputs %s | %FileCheck %s

// This module contains an enum that gets imported by the compiler as an
// OptionSet.  What we're trying to test here is that a *non-resilient*
// synthesized protocol conformance works correctly; in particular, it needs
// to emit a GenericWitnessTable in the ProtocolConformanceDescriptor so that
// the initializer for the protocol witness table actually runs.
// (see rdar://97290618)

import SynthesizedProtocol

// CHECK: @"$sSo5Flagsas9OptionSetSCMc" = linkonce_odr hidden constant { i32, i32, i32, i32, i16, i16, i32, i32 } { i32 {{(trunc \(i64 )?}}sub ({{i(32|64)}} ptrtoint (ptr @"$ss9OptionSetMp" to {{i(32|64)}}), {{i(32|64)}} ptrtoint (ptr @"$sSo5Flagsas9OptionSetSCMc" to {{i(32|64)}})){{( to i32\))?}}, i32 {{(trunc \(i64 )?}}sub ({{i(32|64)}} ptrtoint (ptr @"$sSo5FlagsaMn" to {{i(32|64)}}), {{i(32|64)}} ptrtoint (ptr getelementptr inbounds ({ i32, i32, i32, i32, i16, i16, i32, i32 }, ptr @"$sSo5Flagsas9OptionSetSCMc", i32 0, i32 1) to {{i(32|64)}})){{( to i32\))?}}, i32 {{(trunc \(i64 )?}}sub ({{i(32|64)}} ptrtoint (ptr @"$sSo5Flagsas9OptionSetSCWP" to {{i(32|64)}}), {{i(32|64)}} ptrtoint (ptr getelementptr inbounds ({ i32, i32, i32, i32, i16, i16, i32, i32 }, ptr @"$sSo5Flagsas9OptionSetSCMc", i32 0, i32 2) to {{i(32|64)}})){{( to i32\))?}}, i32 131200, i16 3, i16 1, i32 {{(trunc \(i64 )?}}sub ({{i(32|64)}} ptrtoint (ptr @"$sSo5Flagsas9OptionSetSCWI" to {{i(32|64)}}), {{i(32|64)}} ptrtoint (ptr getelementptr inbounds ({ i32, i32, i32, i32, i16, i16, i32, i32 }, ptr @"$sSo5Flagsas9OptionSetSCMc", i32 0, i32 6) to {{i(32|64)}})){{( to i32\))?}}, i32 {{(trunc \(i64 )?}}sub ({{i(32|64)}} ptrtoint (ptr @"$sSo5Flagsas9OptionSetSCMcMK" to {{i(32|64)}}), {{i(32|64)}} ptrtoint (ptr getelementptr inbounds ({ i32, i32, i32, i32, i16, i16, i32, i32 }, ptr @"$sSo5Flagsas9OptionSetSCMc", i32 0, i32 7) to {{i(32|64)}})) {{(to i32\) )?}}}, section "{{[^"]*}}"{{(, comdat)?}},{{.*}} align 4

// Triggers the inclusion of the relevant ProtocolConformanceDescriptor
public func doFlags(f: Flags) -> Any
{
  return f.contains(.Two)
}

// Because the standard library is usually resilient by default, we need to
// implement a minimal subset of it for this test (so that it *isn't* resilient).
// (If we use the resilient version, the compiler will always generate the
// GenericWitnessTable because we're resilient, which invalidates this test.)

// .. Precedence ...............................................................

precedencegroup AssignmentPrecedence {
  assignment: true
  associativity: right
}
precedencegroup AdditionPrecedence {
  associativity: left
  higherThan: AssignmentPrecedence
}
precedencegroup MultiplicationPrecedence {
  associativity: left
  higherThan: AdditionPrecedence
}

// .. Operators ................................................................

infix operator &: MultiplicationPrecedence
infix operator |: AdditionPrecedence
infix operator ^: AdditionPrecedence

infix operator &=: AssignmentPrecedence
infix operator ^=: AssignmentPrecedence
infix operator |=: AssignmentPrecedence

// .. ExpressibleByIntegerLiteral ..............................................

public protocol _ExpressibleByBuiltinIntegerLiteral {
  init(_builtinIntegerLiteral value: Builtin.IntLiteral)
}

public protocol ExpressibleByIntegerLiteral {
  associatedtype IntegerLiteralType: _ExpressibleByBuiltinIntegerLiteral
  init(integerLiteral value: IntegerLiteralType)
}

extension ExpressibleByIntegerLiteral
  where Self: _ExpressibleByBuiltinIntegerLiteral {
  @_transparent
  public init(integerLiteral value: Self) {
    self = value
  }
}

// .. (U)Int32 .................................................................

@frozen
public struct Int32 : FixedWidthInteger, _ExpressibleByBuiltinIntegerLiteral {
  public typealias IntegerLiteralType = Int32
  public var _value: Builtin.Int32

  @_transparent
  public init(_builtinIntegerLiteral x: Builtin.IntLiteral) {
    _value = Builtin.s_to_s_checked_trunc_IntLiteral_Int32(x).0
  }

  @_transparent
  public init(bitPattern x: UInt32) {
    _value = x._value
  }

  @_transparent
  public init(_ _value: Builtin.Int32) {
    self._value = _value
  }

  public static func |=(_ x: inout Int32, _ y: Int32) {
    x = Int32(Builtin.or_Int32(x._value, y._value))
  }
  public static func &=(_ x: inout Int32, _ y: Int32) {
    x = Int32(Builtin.and_Int32(x._value, y._value))
  }
  public static func ^=(_ x: inout Int32, _ y: Int32) {
    x = Int32(Builtin.xor_Int32(x._value, y._value))
  }
}

@frozen
public struct UInt32 : FixedWidthInteger, _ExpressibleByBuiltinIntegerLiteral {
  public typealias IntegerLiteralType = UInt32
  public var _value: Builtin.Int32

  @_transparent
  public init(_builtinIntegerLiteral x: Builtin.IntLiteral) {
    _value = Builtin.s_to_u_checked_trunc_IntLiteral_Int32(x).0
  }

  @_transparent
  public init(bitPattern x: Int32) {
    _value = x._value
  }

  @_transparent
  public init(_ _value: Builtin.Int32) {
    self._value = _value
  }

  public static func |=(x: inout UInt32, y: UInt32) {
    x = UInt32(Builtin.or_Int32(x._value, y._value))
  }
  public static func &=(x: inout UInt32, y: UInt32) {
    x = UInt32(Builtin.and_Int32(x._value, y._value))
  }
  public static func ^=(x: inout UInt32, y: UInt32) {
    x = UInt32(Builtin.xor_Int32(x._value, y._value))
  }
}

// .. C types ..................................................................

typealias CInt = Int32
typealias CUnsignedInt = UInt32

// .. SetAlgebra ...............................................................

public protocol SetAlgebra {
  associatedtype Element

  func contains(_ member: Element) -> Any
  __consuming func union(_ other: __owned Self) -> Self
  __consuming func intersection(_ other: Self) -> Self
  __consuming func symmetricDifference(_ other: __owned Self) -> Self
  mutating func insert(_ newMember: __owned Element)
  mutating func remove(_ member: Element)
  mutating func update(with newMember: __owned Element)
  mutating func formUnion(_ other: __owned Self)
  mutating func formIntersection(_ other: Self)
  mutating func formSymmetricDifference(_ other: __owned Self)

  static func |(_ lhs: Self, _ rhs: Self) -> Self
  static func &(_ lhs: Self, _ rhs: Self) -> Self
  static func ^(_ lhs: Self, _ rhs: Self) -> Self
  static func |=(_ lhs: inout Self, _ rhs: Self)
  static func &=(_ lhs: inout Self, _ rhs: Self)
  static func ^=(_ lhs: inout Self, _ rhs: Self)
}

extension SetAlgebra {
  @_transparent
  public static func & (_ lhs: Self, _ rhs: Self) -> Self {
    return lhs.intersection(rhs)
  }

  @_transparent
  public static func | (_ lhs: Self, _ rhs: Self) -> Self {
    return lhs.union(rhs)
  }

  @_transparent
  public static func ^ (_ lhs: Self, _ rhs: Self) -> Self {
    return lhs.symmetricDifference(rhs)
  }

  public static func &=(_ lhs: inout Self, _ rhs: Self) {
    lhs.formIntersection(rhs)
  }

  public static func |=(_ lhs: inout Self, _ rhs: Self) {
    lhs.formUnion(rhs)
  }

  public static func ^=(_ lhs: inout Self, _ rhs: Self) {
    lhs.formSymmetricDifference(rhs)
  }
}

// .. RawRepresentable .........................................................

public protocol RawRepresentable<RawValue> {
  associatedtype RawValue
  init(rawValue: RawValue)
  var rawValue: RawValue { get }
}

// .. OptionSet ................................................................

public protocol OptionSet: SetAlgebra, RawRepresentable {
  associatedtype Element = Self
  init(rawValue: RawValue)
}

extension OptionSet {
  @inlinable
  public func union(_ other: Self) -> Self {
    var r = Self(rawValue: self.rawValue)
    r.formUnion(other)
    return r
  }

  @inlinable
  public func intersection(_ other: Self) -> Self {
    var r = Self(rawValue: self.rawValue)
    r.formIntersection(other)
    return r
  }

  @inlinable
  public func symmetricDifference(_ other: Self) -> Self {
    var r = Self(rawValue: self.rawValue)
    r.formSymmetricDifference(other)
    return r
  }
}

extension OptionSet where Element == Self {
  public func contains(_ member: Self) -> Any {
    return self
  }

  public mutating func insert(_ newMember: Element) {
  }

  public mutating func remove(_ member: Element) {
  }

  public mutating func update(with newMember: Element) {
  }
}

extension OptionSet where RawValue: FixedWidthInteger {
  @inlinable
  public mutating func formUnion(_ other: Self) {
    self = Self(rawValue: self.rawValue | other.rawValue)
  }

  @inlinable
  public mutating func formIntersection(_ other: Self) {
    self = Self(rawValue: self.rawValue & other.rawValue)
  }

  @inlinable
  public mutating func formSymmetricDifference(_ other: Self) {
    self = Self(rawValue: self.rawValue ^ other.rawValue)
  }
}

// .. FixedWidthInteger ........................................................

public protocol FixedWidthInteger {
  static func |(_ lhs: Self, _ rhs: Self) -> Self
  static func &(_ lhs: Self, _ rhs: Self) -> Self
  static func ^(_ lhs: Self, _ rhs: Self) -> Self
  static func |=(_ lhs: inout Self, _ rhs: Self)
  static func &=(_ lhs: inout Self, _ rhs: Self)
  static func ^=(_ lhs: inout Self, _ rhs: Self)
}

extension FixedWidthInteger {
  @_transparent
  public static func & (_ lhs: Self, _ rhs: Self) -> Self {
    var lhs = lhs
    lhs &= rhs
    return lhs
  }

  @_transparent
  public static func | (_ lhs: Self, _ rhs: Self) -> Self {
    var lhs = lhs
    lhs |= rhs
    return lhs
  }

  @_transparent
  public static func ^ (_ lhs: Self, _ rhs: Self) -> Self {
    var lhs = lhs
    lhs ^= rhs
    return lhs
  }
}

// .. ExpressibleByArrayLiteral ................................................

public protocol ExpressibleByArrayLiteral {
}

// .. Equatable ................................................................

public protocol Equatable {
}
