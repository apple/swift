// RUN: %target-swift-frontend -emit-sil %s -verify

// Closures
var c1 = {() throws -> Int in 0}
var c2 : () throws -> Int = c1
var c3 : () -> Int = { do { _ = try c2()  } catch _ { var x = 0 } ; return 0 } // expected-warning {{initialization of variable 'x' was never used; consider replacing with assignment to '_' or removing it}}
