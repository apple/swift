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
/// the ``Observation/Observable()`` macro to indicate observability of a type.
@available(SwiftStdlib 5.9, *)
public struct ObservationRegistrar: Sendable {
  internal class ValueObservationStorage {
    func emit<Element>(_ element: Element) -> Bool { return false }
    func cancel() { }
  }
  
  private struct ValuesObserver {
    private let storage: ValueObservationStorage
    
    internal init(storage: ValueObservationStorage) {
      self.storage = storage
    }
    
    internal func emit<Element>(_ element: Element) -> Bool {
      storage.emit(element)
    }
    
    internal func cancel() {
      storage.cancel()
    }
  }
  
  private struct State: @unchecked Sendable {
    private enum ObservationKind {
      case willSetTracking(@Sendable () -> Void)
      case didSetTracking(@Sendable () -> Void)
      case computed(@Sendable (Any) -> Void)
      case values(ValuesObserver)
    }
    
    private struct Observation {
      private var kind: ObservationKind
      internal var properties: Set<AnyKeyPath>
      
      internal init(kind: ObservationKind, properties: Set<AnyKeyPath>) {
        self.kind = kind
        self.properties = properties
      }
      
      var willSetTracker: (@Sendable () -> Void)? {
        switch kind {
        case .willSetTracking(let tracker):
          return tracker
        default:
          return nil
        }
      }

      var didSetTracker: (@Sendable () -> Void)? {
        switch kind {
        case .didSetTracking(let tracker):
          return tracker
        default:
          return nil
        }
      }
      
      var observer: (@Sendable (Any) -> Void)? {
        switch kind {
        case .computed(let observer):
          return observer
        default:
          return nil
        }
      }
      
      var isValueObserver: Bool {
        switch kind {
        case .values:
          return true
        default:
          return false
        }
      }
      
      func emit<Element>(_ value: Element) -> Bool {
        switch kind {
        case .values(let observer):
          return observer.emit(value)
        default:
          return false
        }
      }
      
      func cancel() {
        switch kind {
        case .values(let observer):
          observer.cancel()
        default:
          break
        }
      }
    }
    
    private var id = 0
    private var observations = [Int : Observation]()
    private var lookups = [AnyKeyPath : Set<Int>]()
    
    internal mutating func generateId() -> Int {
      defer { id &+= 1 }
      return id
    }
    
    internal mutating func registerTracking(for properties: Set<AnyKeyPath>, willSet observer: @Sendable @escaping () -> Void) -> Int {
      let id = generateId()
      observations[id] = Observation(kind: .willSetTracking(observer), properties: properties)
      for keyPath in properties {
        lookups[keyPath, default: []].insert(id)
      }
      return id
    }

    internal mutating func registerTracking(for properties: Set<AnyKeyPath>, didSet observer: @Sendable @escaping () -> Void) -> Int {
      let id = generateId()
      observations[id] = Observation(kind: .didSetTracking(observer), properties: properties)
      for keyPath in properties {
        lookups[keyPath, default: []].insert(id)
      }
      return id
    }
    
    internal mutating func registerComputedValues(for properties: Set<AnyKeyPath>, observer: @Sendable @escaping (Any) -> Void) -> Int {
      let id = generateId()
      observations[id] = Observation(kind: .computed(observer), properties: properties)
      for keyPath in properties {
        lookups[keyPath, default: []].insert(id)
      }
      return id
    }
    
    internal mutating func registerValues(for properties: Set<AnyKeyPath>, storage: ValueObservationStorage) -> Int {
      let id = generateId()
      observations[id] = Observation(kind: .values(ValuesObserver(storage: storage)), properties: properties)
      for keyPath in properties {
        lookups[keyPath, default: []].insert(id)
      }
      return id
    }
    
    internal func valueObservers(for keyPath: AnyKeyPath) -> Set<Int> {
      guard let ids = lookups[keyPath] else {
        return []
      }
      return ids.filter { observations[$0]?.isValueObserver == true }
    }
    
    internal mutating func cancel(_ id: Int) {
      if let observation = observations.removeValue(forKey: id) {
        for keyPath in observation.properties {
          if var ids = lookups[keyPath] {
            ids.remove(id)
            if ids.count == 0 {
              lookups.removeValue(forKey: keyPath)
            } else {
              lookups[keyPath] = ids
            }
          }
        }
        observation.cancel()
      }
    }

    internal mutating func cancelAll() {
      for observation in observations.values {
        observation.cancel()
      }
      observations.removeAll()
      lookups.removeAll()
    }
    
    internal mutating func willSet(keyPath: AnyKeyPath) -> [@Sendable () -> Void] {
      var trackers = [@Sendable () -> Void]()
      if let ids = lookups[keyPath] {
        for id in ids {
          if let tracker = observations[id]?.willSetTracker {
            trackers.append(tracker)
          }
        }
      }
      return trackers
    }
    
    internal mutating func didSet<Subject: Observable, Member>(keyPath: KeyPath<Subject, Member>) -> ([@Sendable (Any) -> Void], [@Sendable () -> Void]) {
      var observers = [@Sendable (Any) -> Void]()
      var trackers = [@Sendable () -> Void]()
      if let ids = lookups[keyPath] {
        for id in ids {
          if let observer = observations[id]?.observer {
            observers.append(observer)
            cancel(id)
          }
          if let tracker = observations[id]?.didSetTracker {
            trackers.append(tracker)
          }
        }
      }
      return (observers, trackers)
    }
    
    internal mutating func emit<Element>(_ value: Element, ids: Set<Int>) {
      for id in ids {
        if observations[id]?.emit(value) == true {
          cancel(id)
        }
      }
    }
  }
  
  internal struct Context: Sendable {
    private let state = _ManagedCriticalState(State())
    
    internal var id: ObjectIdentifier { state.id }
    
    internal func registerTracking(for properties: Set<AnyKeyPath>, willSet observer: @Sendable @escaping () -> Void) -> Int {
      state.withCriticalRegion { $0.registerTracking(for: properties, willSet: observer) }
    }

    internal func registerTracking(for properties: Set<AnyKeyPath>, didSet observer: @Sendable @escaping () -> Void) -> Int {
      state.withCriticalRegion { $0.registerTracking(for: properties, didSet: observer) }
    }
    
    internal func registerComputedValues(for properties: Set<AnyKeyPath>, observer: @Sendable @escaping (Any) -> Void) -> Int {
      state.withCriticalRegion { $0.registerComputedValues(for: properties, observer: observer) }
    }
    
    internal func registerValues(for properties: Set<AnyKeyPath>, storage: ValueObservationStorage) -> Int {
      state.withCriticalRegion { $0.registerValues(for: properties, storage: storage) }
    }
    
    internal func cancel(_ id: Int) {
      state.withCriticalRegion { $0.cancel(id) }
    }

    internal func cancelAll() {
      state.withCriticalRegion { $0.cancelAll() }
    }
    
    internal func willSet<Subject: Observable, Member>(
       _ subject: Subject,
       keyPath: KeyPath<Subject, Member>
    ) {
      let tracking = state.withCriticalRegion { $0.willSet(keyPath: keyPath) }
      for action in tracking {
        action()
      }
    }
    
    internal func didSet<Subject: Observable, Member>(
      _ subject: Subject,
      keyPath: KeyPath<Subject, Member>
    ) {
      let (ids, (actions, tracking)) = state.withCriticalRegion { ($0.valueObservers(for: keyPath), $0.didSet(keyPath: keyPath)) }
      if !ids.isEmpty {
        let value = subject[keyPath: keyPath]
        state.withCriticalRegion { $0.emit(value, ids: ids) }
      }
      for action in tracking {
        action()
      }
      for action in actions {
        action(subject)
      }
    }
  }

  private final class Extent: @unchecked Sendable {
    let context = Context()

    init() {
    }

    deinit {
      context.cancelAll()
    }
  }
  
  internal var context: Context {
    return extent.context
  }
  
  private var extent = Extent()

  /// Creates an instance of the observation registrar.
  ///
  /// You don't need to create an instance of
  /// ``Observation/ObservationRegistrar`` when using the
  /// ``Observation/Observable()`` macro to indicate observably
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
    context.didSet(subject, keyPath: keyPath)
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
