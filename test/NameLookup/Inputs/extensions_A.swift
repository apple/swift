public struct X { }

public protocol P { }

public struct Y<T> { }

extension Y: P where T: P { }

public struct Z: P { }
