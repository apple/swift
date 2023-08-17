public protocol DefaultsBridge {
  associatedtype T
}

public struct BridgeStructSerializable<T: Codable>: DefaultsBridge {
}

public struct BridgeStructRawRepresentable<T: RawRepresentable>: DefaultsBridge {
}

public protocol DefaultsSerializable {
  associatedtype Bridge: DefaultsBridge
  static var _defaults: Bridge { get }
}

public extension DefaultsSerializable where Self: Codable {
  static var _defaults: BridgeStructSerializable<Self> { return BridgeStructSerializable() }
}

public extension DefaultsSerializable where Self: RawRepresentable {
  static var _defaults: BridgeStructRawRepresentable<Self> { return BridgeStructRawRepresentable() }
}

public struct ModuleAFoo: Codable, DefaultsSerializable {
}

public struct AliasTest<T> {
  public typealias A = T.Element where T: Collection
}

extension AliasTest where T: Collection {
  public typealias B = T.Element
}
