// RUN: %target-swift-emit-silgen %s -disable-availability-checking

struct Variadic<each T> where repeat each T: Equatable {}

_ = Variadic<Int, String>()
