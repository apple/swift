public struct ImplicitEquatable: ExpressibleByIntegerLiteral, Equatable {
  public init(integerLiteral: Int) {}
}

public struct ExplicitEquatable: ExpressibleByIntegerLiteral, Equatable {
  public init(integerLiteral: Int) {}

  public static func == (lhs: ExplicitEquatable, rhs: ExplicitEquatable) -> Bool {
    return true
  }
}
