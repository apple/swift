@frozen
public struct SomeData {
  public init() {}
}

public final class SomeClass: Sendable {
  public init() {}
}

// Helper to prevent return values from getting optimized away
@inline(never)
public func consume<T>(_ x: T) {
  withExtendedLifetime(x) {}
}

@_specialize(exported: true, where T == Int)
@_specialize(exported: true, where T == Double)
@_specialize(exported: true, where @_noMetadata T : _Class)
@_specialize(exported: true, where @_noMetadata T : _BridgeObject)
@_specialize(exported: true, where @_noMetadata T : _Trivial(64))
@_specialize(exported: true, where @_noMetadata T : _TrivialStride(96, 32))
@_specialize(exported: true, availability: macOS 10.50, *; where T == SomeData)
public func publicPrespecialized<T>(_ t: T) -> T {
  return t
}

@_specialize(exported: true, where @_noMetadata T : _Class)
public func publicPrespecializedWithMarkerProtocol<T: Sendable>(_ t: T) -> T {
  return t
}

@_specialize(exported: true, where T == Int)
@_specialize(exported: true, where @_noMetadata T : _Class)
@_specialize(exported: true, availability: macOS 10.50, *; where T == SomeData)
@inlinable
@inline(never)
public func publicPrespecialized2<T>(_ t: T) { }

@_specialize(exported: true, where T == Int)
@_specialize(exported: true, where @_noMetadata T : _Class)
@_specialize(exported: true, availability: macOS 10.50, *; where T == SomeData)
@inlinable
@inline(never)
public func publicPrespecializedThrows<T>(_ t: T) throws -> T { return t }

@_specialize(exported: true, where T == Int)
@_specialize(exported: true, where T == Double)
@_specialize(exported: true, where @_noMetadata T : _Class)
@_alwaysEmitIntoClient
@inline(never)
internal func internalEmitIntoClientPrespecialized<T>(_ t: T) {
}

@inlinable
public func useInternalEmitIntoClientPrespecialized<T>(_ t: T) {
  internalEmitIntoClientPrespecialized(t)
}

@inlinable
public func publicInlineable<T>(_ t: T) {
}

public struct ResilientThing {
  public init() {}
}

@usableFromInline
@frozen
internal struct InternalThing2<T> {
  @usableFromInline
  var x : T

  @usableFromInline
  init(_ t: T) {
    x = t
  }

  @_specialize(exported: true, where T == Int)
  @_specialize(exported: true, where @_noMetadata T : _Class)
  @inlinable
  func compute() -> T {
    return x
  }

  @inlinable
  var computedX : T {
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    get {
    return x
    }
  }

  @inlinable
  var computedY : T {
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    get {
      return x
    }
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    set {
      x = newValue
    }
  }

  @inlinable
  var computedZ : T {
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    _modify {
      yield &x
    }
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    _read {
      yield x
    }
  }
  @inlinable
  subscript(_ i: Int) -> T {
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    get {
      return x
    }
    @_specialize(exported: true, where T == Int)
    @_specialize(exported: true, where @_noMetadata T : _Class)
    set {
    }
  }
}

@inlinable
public func useInternalThing<T>(_ t: T) {
  var x = InternalThing2(t)
  print(x.compute())
  print(x.computedX)
  x.computedY = t
  print(x.computedY)
  x.computedZ = t
  print(x.computedZ)
  x[1] = t
  print(x[1])
}

@_specialize(exported: true, where @_noMetadata T : _Class, @_noMetadata V : _Class)
@_specialize(exported: true, where @_noMetadata T : _BridgeObject, @_noMetadata V : _BridgeObject)
@_specialize(exported: true, where @_noMetadata T : _TrivialStride(96, 32), @_noMetadata V : _TrivialStride(96, 32))
public func publicPresepcializedMultipleIndirectResults<T, V>(_ t: T, _ v: V, _ x: Int64) -> (V, Int64, T) {
    return (v, x, t)
}

@_specialize(exported: true, where @_noMetadata T : _Class, @_noMetadata V : _Class)
public func publicPresepcializedMultipleIndirectResultsWithMarkerProtocol<T: Sendable, V>(_ t: T, _ v: V, _ x: Int64) -> (V, Int64, T) {
    return (v, x, t)
}
