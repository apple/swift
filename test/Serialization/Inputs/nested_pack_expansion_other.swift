public typealias A<each T, U, V> = (repeat (each T, U, V))

public struct G<each T> {
  public typealias B<each U, V> = (repeat A<repeat each T, each U, V>)

  public struct H<each U> {
    public typealias C<each V> = (repeat B<repeat each U, each V>)
  }
}
