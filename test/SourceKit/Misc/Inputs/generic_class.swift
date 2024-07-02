
final public class X<T> {
  var x: T

  init(_ t: T) { x = t}

  public func foo() -> T { x }
}

