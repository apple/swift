// Fixed-layout struct
@frozen public struct Point {
  public var x: Int // read-write stored property
  public let y: Int // read-only stored property

  public init(x: Int, y: Int) {
    self.x = x
    self.y = y
  }

  public func method() {}
  public mutating func mutantMethod() {}
}

// Resilient-layout struct
public struct Size {
  public var w: Int // should have getter and setter
  public let h: Int // getter only

  public init(w: Int, h: Int) {
    self.w = w
    self.h = h
  }

  public func method() {}
  public mutating func mutantMethod() {}
}

// Fixed-layout struct with resilient members
@frozen public struct Rectangle {
  public let p: Point
  public let s: Size
  public let color: Int

  public init(p: Point, s: Size, color: Int) {
    self.p = p
    self.s = s
    self.color = color
  }
}

// More complicated resilient structs for runtime tests
public struct ResilientBool {
  public let b: Bool

  public init(b: Bool) {
    self.b = b
  }
}

public struct ResilientInt {
  public let i: Int

  public init(i: Int) {
    self.i = i
  }
}

public struct ResilientDouble {
  public let d: Double

  public init(d: Double) {
    self.d = d
  }
}

@frozen public struct ResilientLayoutRuntimeTest {
  public let b1: ResilientBool
  public let i: ResilientInt
  public let b2: ResilientBool
  public let d: ResilientDouble

  public init(b1: ResilientBool, i: ResilientInt, b2: ResilientBool, d: ResilientDouble) {
    self.b1 = b1
    self.i = i
    self.b2 = b2
    self.d = d
  }
}

public class Referent {
  public init() {}
}

public struct ResilientWeakRef {
  public weak var ref: Referent?

  public init (_ r: Referent) {
    ref = r
  }
}

public struct ResilientRef {
  public var r: Referent

  public init(r: Referent) { self.r = r }
}

public struct ResilientWithInternalField {
  var x: Int
}

// Tuple parameters with resilient structs
public class Subject {}

public struct Container {
  public var s: Subject
}

public struct PairContainer {
  public var pair : (Container, Container)
}

@available(*, unavailable)
public struct UnavailableResilientInt {
  public let i: Int

  public init(i: Int) {
    self.i = i
  }
}
