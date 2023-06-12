// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/Inputs/nested_pack_expansion_other.swift -emit-module -emit-module-path %t/nested_pack_expansion_other.swiftmodule -disable-availability-checking
// RUN: %target-typecheck-verify-swift -I %t -disable-availability-checking

import nested_pack_expansion_other

func sameType<T>(_: T.Type, _: T.Type) {}

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

