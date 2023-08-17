public struct G<T: Sendable> {
}

public func makeG<T: Sendable>(_: T.Type) -> G<T> {
  return G<T>()
}

@_transparent public func makeVoidG() -> G<Void> {
  return makeG(Void.self)
}
