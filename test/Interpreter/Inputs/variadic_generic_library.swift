public struct Key: Hashable {
  static var index = 0

  public let id: Int

  init() {
    self.id = Self.index
    Self.index += 1
  }
}

public struct Variable<Value> {
  public let key = Key()
}

public struct Bindings {
  private var storage: [Key : Any] = [:]

  public init<each T>(
    _ value: repeat (Variable<each T>, each T)
  ) {
    _ = (repeat storage[(each value).0.key] = (each value).1)
  }
}

public protocol Expression<Result> {
  associatedtype Result
  func evaluate(_ bindings: Bindings) throws -> Result
}

public struct True: Expression {
  public init() {}

  public func evaluate(_ bindings: Bindings) throws -> Bool {
    true
  }
}

public struct Predicate<each Input> {
  var variables: (repeat Variable<each Input>)
  var expression: any Expression<Bool>

  public init<Expr>(
    builder: (repeat Variable<each Input>) -> Expr
  ) where Expr: Expression<Bool> {
    self.variables = (repeat Variable<each Input>())
    self.expression = builder(repeat each variables)
  }

  public func evaluate(
    _ input: repeat each Input
  ) throws -> Bool  {
    return try expression.evaluate(
      .init(repeat (each variables, each input))
    )
  }
}

extension Sequence {
  public func filter(_ predicate: Predicate<Element>) throws -> [Element] {
    try self.filter {
      try predicate.evaluate($0)
    }
  }
}
