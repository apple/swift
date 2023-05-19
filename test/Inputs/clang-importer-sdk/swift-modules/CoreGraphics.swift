@_exported import ObjectiveC
@_exported import CoreGraphics

public func == (lhs: CGPoint, rhs: CGPoint) -> Bool {
  return lhs.x == rhs.x  &&  lhs.y == rhs.y
}

#if !CGFLOAT_IN_COREFOUNDATION
public struct CGFloat {
#if _pointerBitWidth(_32)
  public typealias UnderlyingType = Float
#elseif _pointerBitWidth(_64)
  public typealias UnderlyingType = Double
#else
#error("Unknown platform")
#endif

  public init() { 
    self.value = 0.0
  }

  public init(_ value: Int) { 
    self.value = UnderlyingType(value)
  }

  public init(_ value: Float) { 
    self.value = UnderlyingType(value)
  }

  public init(_ value: Double) { 
    self.value = UnderlyingType(value)
  }

  var value: UnderlyingType
}

public func ==(lhs: CGFloat, rhs: CGFloat) -> Bool {
  return lhs.value == rhs.value
}

extension CGFloat : ExpressibleByIntegerLiteral, ExpressibleByFloatLiteral, Equatable {
  public init(integerLiteral value: UnderlyingType) {
    self.value = value
  }

  public init(floatLiteral value: UnderlyingType) {
    self.value = value
  }
}

public extension Double {
  init(_ value: CGFloat) {
    self = Double(value.value)
  }
}
#endif

import CoreFoundation

extension CGFloat: CustomStringConvertible {
  public var description: String { "" }
}
