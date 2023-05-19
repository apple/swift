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
public struct ObservationTracking {
  struct Entry: @unchecked Sendable {
    let registerTracking: @Sendable (Set<AnyKeyPath>, @Sendable @escaping () -> Void) -> Int
    let cancel: @Sendable (Int) -> Void
    var properties = Set<AnyKeyPath>()
    
    init(_ context: ObservationRegistrar.Context) {
      registerTracking = { properties, observer in
        context.registerTracking(for: properties, observer: observer)
      }
      cancel = { id in
        context.cancel(id)
      }
    }
    
    func addObserver(_ changed: @Sendable @escaping () -> Void) -> Int {
      return registerTracking(properties, changed)
    }
    
    func removeObserver(_ token: Int) {
      cancel(token)
    }
    
    mutating func insert(_ keyPath: AnyKeyPath) {
      properties.insert(keyPath)
    }
    
    mutating func formUnion(_ properties: Set<AnyKeyPath>) {
      self.properties.formUnion(properties)
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
      for (identifier, entry) in other.entries {
        entries[identifier, default: entry].formUnion(entry.properties)
      }
    }
  }

  @_spi(SwiftUI)
  public static func _installTracking(
    _ list: _AccessList,
    onChange: @escaping @Sendable () -> Void
  ) {
    let state = _ManagedCriticalState([ObjectIdentifier: Int]())
    let values = list.entries.mapValues { $0.addObserver {
      onChange()
      let values = state.withCriticalRegion { $0 }
      for (id, token) in values {
        list.entries[id]?.removeObserver(token)
      }
    }}
    state.withCriticalRegion { $0 = values }
  }
}

@available(SwiftStdlib 5.9, *)
public func withObservationTracking<T>(
  _ apply: () -> T,
  onChange: @autoclosure () -> @Sendable () -> Void
) -> T {
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
  if let list = accessList {
    let state = _ManagedCriticalState([ObjectIdentifier: Int]())
    let onChange = onChange()
    let values = list.entries.mapValues { $0.addObserver {
      onChange()
      let values = state.withCriticalRegion { $0 }
      for (id, token) in values {
        list.entries[id]?.removeObserver(token)
      }
    }}
    state.withCriticalRegion { $0 = values }
  }
  return result
}
