//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
public protocol AtomicProtocol {
  associatedtype AtomicStorage

  static func atomicStorage(for value: Self) -> AtomicStorage

  static func deinitializeAtomicStorage(
    at pointer: UnsafeMutablePointer<AtomicStorage>
  )

  @_semantics("has_constant_evaluable_arguments")
  static func atomicLoad(
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicLoadOrdering
  ) -> Self

  @_semantics("has_constant_evaluable_arguments")
  static func atomicStore(
    _ desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicStoreOrdering
  )

  @_semantics("has_constant_evaluable_arguments")
  static func atomicExchange(
    _ desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering
  ) -> Self

  @_semantics("has_constant_evaluable_arguments")
  static func atomicCompareExchange(
    expected: Self,
    desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering
  ) -> (exchanged: Bool, original: Self)

  @_semantics("has_constant_evaluable_arguments")
  static func atomicCompareExchange(
    expected: Self,
    desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering,
    failureOrdering: AtomicLoadOrdering
  ) -> (exchanged: Bool, original: Self)
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension AtomicProtocol where AtomicStorage == Self {
  @inlinable
  public static func atomicStorage(for value: Self) -> AtomicStorage {
    return value
  }

  @inlinable
  public static func deinitializeAtomicStorage(
    at pointer: UnsafeMutablePointer<AtomicStorage>
  ) {
    pointer.deinitialize(count: 1)
  }
}

@available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *)
extension AtomicProtocol where
  Self: RawRepresentable,
  RawValue: AtomicProtocol,
  AtomicStorage == RawValue.AtomicStorage
{
  @inlinable
  public static func atomicStorage(for value: Self) -> RawValue.AtomicStorage {
    return RawValue.atomicStorage(for: value.rawValue)
  }

  @inlinable
  public static func deinitializeAtomicStorage(
    at pointer: UnsafeMutablePointer<AtomicStorage>
  ) {
    pointer.deinitialize(count: 1)
  }

  @_semantics("has_constant_evaluable_arguments")
  @_transparent @_alwaysEmitIntoClient
  public static func atomicLoad(
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicLoadOrdering
  ) -> Self {
    let raw = RawValue.atomicLoad(at: pointer, ordering: ordering)
    return Self(rawValue: raw)!
  }

  @_semantics("has_constant_evaluable_arguments")
  @_transparent @_alwaysEmitIntoClient
  public static func atomicStore(
    _ desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicStoreOrdering
  ) {
    RawValue.atomicStore(
      desired.rawValue,
      at: pointer,
      ordering: ordering)
  }

  @_semantics("has_constant_evaluable_arguments")
  @_transparent @_alwaysEmitIntoClient
  public static func atomicExchange(
    _ desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering
  ) -> Self {
    let raw = RawValue.atomicExchange(
      desired.rawValue,
      at: pointer,
      ordering: ordering)
    return Self(rawValue: raw)!
  }

  @_semantics("has_constant_evaluable_arguments")
  @_transparent @_alwaysEmitIntoClient
  public static func atomicCompareExchange(
    expected: Self,
    desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering
  ) -> (exchanged: Bool, original: Self) {
    let (exchanged, raw) = RawValue.atomicCompareExchange(
      expected: expected.rawValue,
      desired: desired.rawValue,
      at: pointer,
      ordering: ordering)
    return (exchanged, Self(rawValue: raw)!)
  }

  @_semantics("has_constant_evaluable_arguments")
  @_transparent @_alwaysEmitIntoClient
  public static func atomicCompareExchange(
    expected: Self,
    desired: Self,
    at pointer: UnsafeMutablePointer<AtomicStorage>,
    ordering: AtomicUpdateOrdering,
    failureOrdering: AtomicLoadOrdering
  ) -> (exchanged: Bool, original: Self) {
    let (exchanged, raw) = RawValue.atomicCompareExchange(
      expected: expected.rawValue,
      desired: desired.rawValue,
      at: pointer,
      ordering: ordering,
      failureOrdering: failureOrdering)
    return (exchanged, Self(rawValue: raw)!)
  }
}
