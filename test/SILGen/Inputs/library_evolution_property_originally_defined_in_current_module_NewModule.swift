@available(macOS 10.15, *)
@_originallyDefinedIn(module: "OldModule", macOS 12.0)
public struct OldModuleType {
    public let value: UInt32

    public static let property = OldModuleType(value: 0xFFFF_FFFF)

    public init(value: UInt32) {
      self.value = value
    }
}
