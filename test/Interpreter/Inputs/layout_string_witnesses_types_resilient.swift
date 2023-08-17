
public struct SimpleResilient {
    let x: Int
    let y: AnyObject

    public init(x: Int, y: AnyObject) {
        self.x = x
        self.y = y
    }
}

public struct GenericResilient<C, T> {
    let x: C
    let y: T

    public init(x: C, y: T) {
        self.x = x
        self.y = y
    }
}

public enum ResilientSinglePayloadEnumGeneric<T> {
    case empty0
    case empty1
    case nonEmpty(T)
}

public enum ResilientMultiPayloadEnumGeneric<T> {
    case empty0
    case empty1
    case nonEmpty0(AnyObject)
    case nonEmpty1(T)
}

public enum ResilientMultiPayloadEnum {
    case empty0
    case empty1
    case nonEmpty0(AnyObject, Bool)
    case nonEmpty1(AnyObject)
}

public enum ResilientSinglePayloadEnumComplex {
    case empty0
    case empty1
    case nonEmpty(ResilientMultiPayloadEnum)
}

public enum ResilientSinglePayloadEnumSimple {
    case empty0
    case empty1
    case nonEmpty(AnyObject)
}

public enum ResilientSingletonEnum {
    case nonEmpty(AnyObject)
}

public func getResilientSinglePayloadEnumGenericEmpty0<T>(_ t: T.Type) -> ResilientSinglePayloadEnumGeneric<T> {
    return .empty0
}

public func getResilientMultiPayloadEnumGenericEmpty0<T>(_ t: T.Type) -> ResilientMultiPayloadEnumGeneric<T> {
    return .empty0
}

public func getResilientMultiPayloadEnumEmpty0() -> ResilientMultiPayloadEnum {
    return .empty0
}

public func getResilientSinglePayloadEnumComplexEmpty0() -> ResilientSinglePayloadEnumComplex {
    return .empty0
}

public func getResilientSinglePayloadEnumSimpleEmpty0() -> ResilientSinglePayloadEnumSimple {
    return .empty0
}

public func getResilientSingletonEnumNonEmpty(_ x: AnyObject) -> ResilientSingletonEnum {
    return .nonEmpty(x)
}

public enum ResilientSinglePayloadEnum {
    case empty0
    case empty1
    case nonEmpty(AnyObject, Int)
}
