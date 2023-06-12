// RUN: %target-typecheck-verify-swift -disable-availability-checking

func sameType<T>(_: T.Type, _: T.Type) {}

typealias A<each T, U, V> = (repeat (each T, U, V))

struct G<each T> {
  typealias B<each U, V> = (repeat A<repeat each T, each U, V>)

  struct H<each U> {
    typealias C<each V> = (repeat B<repeat each U, each V>)
  }
}

func f1<each T>(u: repeat G<each T>) {}

func fa<each T, U, V>(t: repeat each T, u: U, v: V) -> A<repeat each T, U, V> {
  return (repeat (each t, u, v))
}

/* FIXME: These don't work yet
func fb<each T, each U, V>(t: repeat each T, u: repeat each U, v: V) -> G<repeat each T>.B<repeat each U, V> {
  return (repeat fa(t: repeat each t, u: each u, v))
}

func fc<each T, each U, each V>(t: repeat each T, u: repeat each U, v: repeat each V) -> G<repeat each T>.H<repeat each U>.C<repeat each V> {
  return (repeat fb(t: repeat each t, u: repeat each u, v: each v))
}
*/

struct X1 {}
struct Y1 {}
struct Z1 {}

struct X2 {}
struct Y2 {}
struct Z2 {}

struct X3 {}
struct Y3 {}
struct Z3 {}

typealias Expanded = ((((X1, X2, X3),
                        (Y1, X2, X3),
                        (Z1, X2, X3)),
                       ((X1, Y2, X3),
                        (Y1, Y2, X3),
                        (Z1, Y2, X3)),
                       ((X1, Z2, X3),
                        (Y1, Z2, X3),
                        (Z1, Z2, X3))),
                      (((X1, X2, Y3),
                        (Y1, X2, Y3),
                        (Z1, X2, Y3)),
                       ((X1, Y2, Y3),
                        (Y1, Y2, Y3),
                        (Z1, Y2, Y3)),
                       ((X1, Z2, Y3),
                        (Y1, Z2, Y3),
                        (Z1, Z2, Y3))),
                      (((X1, X2, Z3),
                        (Y1, X2, Z3),
                        (Z1, X2, Z3)),
                       ((X1, Y2, Z3),
                        (Y1, Y2, Z3),
                        (Z1, Y2, Z3)),
                       ((X1, Z2, Z3),
                        (Y1, Z2, Z3),
                        (Z1, Z2, Z3))))

sameType(G<X1, Y1, Z1>.H<X2, Y2, Z2>.C<X3, Y3, Z3>.self, Expanded.self)

typealias C1<each T> = G<repeat each T>.H<X2, Y2, Z2>.C<X3, Y3, Z3>
typealias C2<each U> = G<X1, Y1, Z1>.H<repeat each U>.C<X3, Y3, Z3>
typealias C3<each V> = G<X1, Y1, Z1>.H<X2, Y2, Z2>.C<repeat each V>

sameType(C1<X1, Y1, Z1>.self, Expanded.self)
sameType(C2<X2, Y2, Z2>.self, Expanded.self)
sameType(C2<X2, Y2, Z2>.self, Expanded.self)

extension G {
  typealias C1<each U> = G<repeat each T>.H<repeat each U>.C<X3, Y3, Z3>
  typealias C2<each V> = G<repeat each T>.H<X2, Y2, Z2>.C<repeat each V>
  typealias C3<each V> = G<X1, Y1, Z1>.H<repeat each T>.C<repeat each V>
}

sameType(G<X1, Y1, Z1>.C1<X2, Y2, Z2>.self, Expanded.self)
sameType(G<X1, Y1, Z1>.C2<X3, Y3, Z3>.self, Expanded.self)
sameType(G<X2, Y2, Z2>.C3<X3, Y3, Z3>.self, Expanded.self)