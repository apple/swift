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

@available(SwiftStdlib 5.9, *)
@_spi(SwiftUI)
public struct ObservationTracking: Sendable {
  enum Id {
    case willSet(Int)
    case didSet(Int)
    case full(Int, Int)
  }

  struct Entry: @unchecked Sendable {
    let context: ObservationRegistrar.Context
    
    var properties: Set<AnyKeyPath>
    
    init(_ context: ObservationRegistrar.Context, properties: Set<AnyKeyPath> = []) {
      self.context = context
      self.properties = properties
    }
    
    func addWillSetObserver(_ changed: @Sendable @escaping () -> Void) -> Int {
      return context.registerTracking(for: properties, willSet: changed)
    }

    func addDidSetObserver(_ changed: @Sendable @escaping () -> Void) -> Int {
      return context.registerTracking(for: properties, didSet: changed)
    }
    
    func removeObserver(_ token: Int) {
      context.cancel(token)
    }
    
    mutating func insert(_ keyPath: AnyKeyPath) {
      properties.insert(keyPath)
    }
    
    func union(_ entry: Entry) -> Entry {
      Entry(context, properties: properties.union(entry.properties))
    }
  }
  
  @_spi(SwiftUI)
  public struct _AccessList: Sendable {
    internal var entries = [ObjectIdentifier : Entry]()

    internal init() { }
    
    internal mutating func addAccess<Subject: Observable>(
      keyPath: PartialKeyPath<Subject>,
      context: ObservationRegistrar.Context
    ) {
      entries[context.id, default: Entry(context)].insert(keyPath)
    }
    
    internal mutating func merge(_ other: _AccessList) {
      entries.merge(other.entries) { existing, entry in
        existing.union(entry)
      }
    }
  }

  @_spi(SwiftUI)
  public static func _installTracking(
    _ tracking: ObservationTracking,
    willSet: (@Sendable (ObservationTracking) -> Void)? = nil,
    didSet: (@Sendable (ObservationTracking) -> Void)? = nil
  ) {
    let values = tracking.list.entries.mapValues { 
      switch (willSet, didSet) {
      case (.some(let willSetObserver), .some(let didSetObserver)):
        return Id.full($0.addWillSetObserver {
          willSetObserver(tracking)
        }, $0.addDidSetObserver {
          didSetObserver(tracking)
        })
      case (.some(let willSetObserver), .none):
        return Id.willSet($0.addWillSetObserver {
          willSetObserver(tracking)
        })
      case (.none, .some(let didSetObserver)):
        return Id.didSet($0.addDidSetObserver {
          didSetObserver(tracking)
        })
      case (.none, .none):
        fatalError()
      }  
    }
    
    tracking.install(values)
  }

  @_spi(SwiftUI)
  public static func _installTracking(
    _ list: _AccessList,
    onChange: @escaping @Sendable () -> Void
  ) {
    let tracking = ObservationTracking(list)
    _installTracking(tracking, willSet: { _ in
      onChange()
      tracking.cancel()
    })
  }

  struct State {
    var values = [ObjectIdentifier: ObservationTracking.Id]()
    var cancelled = false
  }
  
  private let state = _ManagedCriticalState(State())
  private let list: _AccessList
  
  @_spi(SwiftUI)
  public init(_ list: _AccessList?) {
    self.list = list ?? _AccessList()
  }

  internal func install(_ values:  [ObjectIdentifier : ObservationTracking.Id]) {
    state.withCriticalRegion {
      if !$0.cancelled {
        $0.values = values
      }
    }
  }

  public func cancel() {
    let values = state.withCriticalRegion {
      $0.cancelled = true
      let values = $0.values
      $0.values = [:]
      return values
    }
    for (id, observationId) in values {
        switch observationId {
        case .willSet(let token):
          list.entries[id]?.removeObserver(token)
        case .didSet(let token):
          list.entries[id]?.removeObserver(token)
        case .full(let willSetToken, let didSetToken):
          list.entries[id]?.removeObserver(willSetToken)
          list.entries[id]?.removeObserver(didSetToken)
        }
      }
  }
}

@available(SwiftStdlib 5.9, *)
fileprivate func generateAccessList<T>(_ apply: () -> T) -> (T, ObservationTracking._AccessList?) {
  var accessList: ObservationTracking._AccessList?
  let result = withUnsafeMutablePointer(to: &accessList) { ptr in
    let previous = _ThreadLocal.value
    _ThreadLocal.value = UnsafeMutableRawPointer(ptr)
    defer {
      if let scoped = ptr.pointee, let previous {
        if var prevList = previous.assumingMemoryBound(to: ObservationTracking._AccessList?.self).pointee {
          prevList.merge(scoped)
          previous.assumingMemoryBound(to: ObservationTracking._AccessList?.self).pointee = prevList
        } else {
          previous.assumingMemoryBound(to: ObservationTracking._AccessList?.self).pointee = scoped
        }
      }
      _ThreadLocal.value = previous
    }
    return apply()
  }
  return (result, accessList)
}

/// Tracks access to properties.
///
/// This method tracks access to any property within the `apply` closure, and
/// informs the caller of value changes made to participating properties by way
/// of the `onChange` closure. For example, the following code tracks changes
/// to the name of cars, but it doesn't track changes to any other property of
/// `Car`:
/// 
///     func render() {
///         withObservationTracking {
///             for car in cars {
///                 print(car.name)
///             }
///         } onChange: {
///             print("Schedule renderer.")
///         }
///     }
///
/// - Parameters:
///     - apply: A closure that contains properties to track.
///     - onChange: The closure invoked when the value of a property changes.
///
/// - Returns: The value that the `apply` closure returns if it has a return
/// value; otherwise, there is no return value.
@available(SwiftStdlib 5.9, *)
public func withObservationTracking<T>(
  _ apply: () -> T,
  onChange: @autoclosure () -> @Sendable () -> Void
) -> T {
  let (result, accessList) = generateAccessList(apply)
  if let accessList {
    ObservationTracking._installTracking(accessList, onChange: onChange())
  }
  return result
}

@available(SwiftStdlib 5.9, *)
@_spi(SwiftUI)
public func withObservationTracking<T>(
  _ apply: () -> T,
  willSet: @escaping @Sendable (ObservationTracking) -> Void,
  didSet: @escaping @Sendable (ObservationTracking) -> Void
) -> T {
  let (result, accessList) = generateAccessList(apply)
  ObservationTracking._installTracking(ObservationTracking(accessList), willSet: willSet, didSet: didSet)
  return result
}

@available(SwiftStdlib 5.9, *)
@_spi(SwiftUI)
public func withObservationTracking<T>(
  _ apply: () -> T,
  willSet: @escaping @Sendable (ObservationTracking) -> Void
) -> T {
  let (result, accessList) = generateAccessList(apply)
  ObservationTracking._installTracking(ObservationTracking(accessList), willSet: willSet, didSet: nil)
  return result
}

@available(SwiftStdlib 5.9, *)
@_spi(SwiftUI)
public func withObservationTracking<T>(
  _ apply: () -> T,
  didSet: @escaping @Sendable (ObservationTracking) -> Void
) -> T {
  let (result, accessList) = generateAccessList(apply)
  ObservationTracking._installTracking(ObservationTracking(accessList), willSet: nil, didSet: didSet)
  return result
}
