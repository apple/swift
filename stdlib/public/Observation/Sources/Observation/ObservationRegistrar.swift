//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

/// Provides storage for tracking and access to data changes.
///
/// You don't need to create an instance of `ObservationRegistrar` when using
/// the ``Observation/Observable-swift.macro`` macro to indicate observability
/// of a type.
@available(SwiftStdlib 5.9, *)
public struct ObservationRegistrar: Sendable {
  struct State: @unchecked Sendable {
    struct Observation {
      var properties: Set<AnyKeyPath>
      var observer: @Sendable () -> Void
    }
    
    var id = 0
    var observations = [Int : Observation]()
    var lookups = [AnyKeyPath : Set<Int>]()
    
    mutating func generateId() -> Int {
      defer { id &+= 1 }
      return id
    }
    
    mutating func registerTracking(for properties: Set<AnyKeyPath>, observer: @Sendable @escaping () -> Void) -> Int {
      let id = generateId()
      observations[id] = Observation(properties: properties, observer: observer)
      for keyPath in properties {
        lookups[keyPath, default: []].insert(id)
      }
      return id
    }
    
    mutating func cancel(_ id: Int) {
      if let tracking = observations.removeValue(forKey: id) {
        for keyPath in tracking.properties {
          if var ids = lookups[keyPath] {
            ids.remove(id)
            if ids.count == 0 {
              lookups.removeValue(forKey: keyPath)
            } else {
              lookups[keyPath] = ids
            }
          }
        }
      }
    }
    
    mutating func willSet(keyPath: AnyKeyPath) -> [@Sendable () -> Void] {
      var observers = [@Sendable () -> Void]()
      if let ids = lookups[keyPath] {
        for id in ids {
          if let observation = observations[id] {
            observers.append(observation.observer)
            cancel(id)
          }
        }
      }
      return observers
    }
  }
  
  struct Context: Sendable {
    let state = _ManagedCriticalState(State())
    
    var id: ObjectIdentifier { state.id }
    
    func registerTracking(for properties: Set<AnyKeyPath>, observer: @Sendable @escaping () -> Void) -> Int {
      state.withCriticalRegion { $0.registerTracking(for: properties, observer: observer) }
    }
    
    func cancel(_ id: Int) {
      state.withCriticalRegion { $0.cancel(id) }
    }
    
    func willSet<Subject, Member>(
       _ subject: Subject,
       keyPath: KeyPath<Subject, Member>
    ) {
      let actions = state.withCriticalRegion { $0.willSet(keyPath: keyPath) }
      for action in actions {
        action()
      }
    }
  }
  
  let context = Context()
  
  /// Creates an instance of the observation registrar.
  ///
  /// You don't need to create an instance of
  /// ``Observation/ObservationRegistrar`` when using the
  /// ``Observation/Observable-swift.macro`` macro to indicate observably
  /// of a type.
  public init() {
  }

  /// Registers access to a specific property for observation.
  ///
  /// - Parameters:
  ///   - subject: An instance of an observable type.
  ///   - keyPath: The key path of an observed property.
  public func access<Subject: Observable, Member>(
      _ subject: Subject,
      keyPath: KeyPath<Subject, Member>
  ) {
    if let trackingPtr = _ThreadLocal.value?
      .assumingMemoryBound(to: ObservationTracking._AccessList?.self) {
      if trackingPtr.pointee == nil {
        trackingPtr.pointee = ObservationTracking._AccessList()
      }
      trackingPtr.pointee?.addAccess(keyPath: keyPath, context: context)
    }
  }
  
  /// A property observation called before setting the value of the subject.
  ///
  /// - Parameters:
  ///     - subject: An instance of an observable type.
  ///     - keyPath: The key path of an observed property.
  public func willSet<Subject: Observable, Member>(
      _ subject: Subject,
      keyPath: KeyPath<Subject, Member>
  ) {
    context.willSet(subject, keyPath: keyPath)
  }
  
  /// A property observation called after setting the value of the subject.
  ///
  /// - Parameters:
  ///   - subject: An instance of an observable type.
  ///   - keyPath: The key path of an observed property.
  public func didSet<Subject: Observable, Member>(
      _ subject: Subject,
      keyPath: KeyPath<Subject, Member>
  ) {
    
  }
  
  /// Identifies mutations to the transactions registered for observers.
  ///
  /// This method calls ``willset(_:keypath:)`` before the mutation. Then it
  /// calls ``didset(_:keypath:)`` after the mutation.
  /// - Parameters:
  ///   - of: An instance of an observable type.
  ///   - keyPath: The key path of an observed property.
  public func withMutation<Subject: Observable, Member, T>(
    of subject: Subject,
    keyPath: KeyPath<Subject, Member>,
    _ mutation: () throws -> T
  ) rethrows -> T {
    willSet(subject, keyPath: keyPath)
    defer { didSet(subject, keyPath: keyPath) }
    return try mutation()
  }
}

@available(SwiftStdlib 5.9, *)
extension ObservationRegistrar: Codable {
  public init(from decoder: any Decoder) throws {
    self.init()
  }
  
  public func encode(to encoder: any Encoder) {
    // Don't encode a registrar's transient state.
  }
}

@available(SwiftStdlib 5.9, *)
extension ObservationRegistrar: Hashable {
  public static func == (lhs: Self, rhs: Self) -> Bool {
    // A registrar should be ignored for the purposes of determining its
    // parent type's equality.
    return true
  }
  
  public func hash(into hasher: inout Hasher) {
    // Don't include a registrar's transient state in its parent type's
    // hash value.
  }
}
