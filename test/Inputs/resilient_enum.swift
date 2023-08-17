import resilient_struct

// Fixed-layout enum with resilient members
@frozen public enum SimpleShape {
  case KleinBottle
  case Triangle(Size)
}

// Fixed-layout enum with resilient members
@frozen public enum Shape {
  case Point
  case Rect(Size)
  case RoundedRect(Size, Size)
}

// Fixed-layout enum with indirect resilient members
@frozen public enum FunnyShape {
  indirect case Parallelogram(Size)
  indirect case Trapezoid(Size)
}

@frozen public enum FullyFixedLayout {
  case noPayload
  case hasPayload(Int)
}

// The enum payload has fixed layout inside this module, but
// resilient layout outside. Make sure we emit the payload
// size in the metadata.

public struct Color {
  public let r: Int, g: Int, b: Int

  public init(r: Int, g: Int, b: Int) {
    self.r = r
    self.g = g
    self.b = b
  }
}

@frozen public enum CustomColor {
  case Black
  case White
  case Custom(Color)
  case Bespoke(Color, Color)
}

// Resilient enum
public enum Medium {
  // Indirect case
  indirect case Pamphlet(Medium)  // -1

  // Case with resilient payload
  case Postcard(Size)             // -2

  // Empty cases
  case Paper                      // 0
  case Canvas                     // 1
}

// Indirect resilient enum
public indirect enum IndirectApproach {
  case Angle(Double)              // -1
}

// Resilient enum with resilient empty payload case
public struct EmptyStruct {
  public init() {}
}

public enum ResilientEnumWithEmptyCase {
  case A                          // 0
  case B                          // 1
  case Empty(EmptyStruct)         // -1
}

public func getResilientEnumWithEmptyCase() -> [ResilientEnumWithEmptyCase] {
  return [.A, .B, .Empty(EmptyStruct())]
}

// Specific enum implementations for executable tests
public enum ResilientEmptyEnum {
  case X
}

public enum ResilientSingletonEnum {
  case X(AnyObject)
}

public enum ResilientSingletonGenericEnum<T> {
  case X(T)
}

public enum ResilientNoPayloadEnum {
  case A
  case B
  case C
}

public enum ResilientSinglePayloadEnum {
  case X(AnyObject)               // -1
  case A                          // 0
  case B                          // 1
  case C                          // 2
}

public enum ResilientSinglePayloadGenericEnum<T> {
  case X(T)                       // -1
  case A                          // 0
  case B                          // 1
  case C                          // 2
}

public class ArtClass {
  public init() {}
}

public enum ResilientMultiPayloadEnum {
  case A
  case B
  case C
  case X(Int)
  case Y(Int)
}

public func makeResilientMultiPayloadEnum(_ n: Int, i: Int)
    -> ResilientMultiPayloadEnum {
  switch i {
  case 0:
    return .A
  case 1:
    return .B
  case 2:
    return .C
  case 3:
    return .X(n)
  case 4:
    return .Y(n)
  default:
    while true {}
  }
}

public enum ResilientMultiPayloadEnumSpareBits {
  case A                          // 0
  case B                          // 1
  case C                          // 2
  case X(ArtClass)                // -1
  case Y(ArtClass)                // -2
}

public func makeResilientMultiPayloadEnumSpareBits(_ o: ArtClass, i: Int)
    -> ResilientMultiPayloadEnumSpareBits {
  switch i {
  case 0:
    return .A
  case 1:
    return .B
  case 2:
    return .C
  case 3:
    return .X(o)
  case 4:
    return .Y(o)
  default:
    while true {}
  }
}

public typealias SevenSpareBits = (Bool, Int8, Int8, Int8, Int8, Int8, Int8, Int8)

public enum ResilientMultiPayloadEnumSpareBitsAndExtraBits {
  // On 64-bit little endian, 7 spare bits at the LSB end
  case P1(SevenSpareBits)
  // On 64-bit, 8 spare bits at the MSB end and 3 at the LSB end
  case P2(ArtClass)
  case P3(ArtClass)
  case P4(ArtClass)
  case P5(ArtClass)
  case P6(ArtClass)
  case P7(ArtClass)
  case P8(ArtClass)
}

public enum ResilientMultiPayloadGenericEnum<T> {
  case A                          // 0
  case B                          // 1
  case C                          // 2
  case X(T)                       // -1
  case Y(T)                       // -2
}

public enum ResilientMultiPayloadGenericEnumFixedSize<T> {
  case A                          // 0
  case B                          // 1
  case C                          // 2
  case X(Int)                     // -1
  case Y(Int)                     // -2
}

public enum ResilientIndirectEnum {
  // 0
  case Base

  // -1
  indirect case A(ResilientIndirectEnum)

  // -2
  indirect case B(ResilientIndirectEnum, ResilientIndirectEnum)
}

public enum ResilientEnumWithUnavailableCase {
  case available

  @available(*, unavailable)
  case unavailable
}

public enum ResilientEnumWithUnavailableCaseAndPayload {
  @available(*, unavailable)
  case int(_ i: UnavailableResilientInt)

  case double(_ d: ResilientDouble)
}

extension ResilientEnumWithUnavailableCaseAndPayload {
  @_alwaysEmitIntoClient
  public var intValue: Int {
    switch self {
    case .int(let i): return i.i
    case .double(let d): return Int(d.d)
    default: return -1
    }
  }
}
