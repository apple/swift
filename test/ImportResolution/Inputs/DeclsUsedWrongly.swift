public var x: Int = 0

public enum Choice {
  case yes, no, maybeSo
}

public typealias Callback = () -> Void

public typealias Pair<T> = (T, T)



public struct NamespaceStruct {

    public protocol NestedProtocol {}

    public typealias AnyNestedProtocol = any NestedProtocol
}

public typealias AnyNestedProtocol = NamespaceStruct.AnyNestedProtocol

public typealias NestedProtocol = NamespaceStruct.NestedProtocol

public protocol TopLevelProtocol {

}

public typealias AnyTopLevelProtocol = any TopLevelProtocol
