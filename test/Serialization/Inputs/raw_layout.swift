public protocol Foo {}
extension Bool: Foo {}

@_rawLayout(like: T)
public struct Fred<T: Foo>: ~Copyable {
  public init() {}
}

@_rawLayout(like: T, movesAsLike)
public struct CellThatMovesLike<T>: ~Copyable {}
