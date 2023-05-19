// RUN: %target-swift-ide-test -print-expr-type -source-filename %S/Inputs/ExprType.swift -swift-version 5 | %FileCheck %s -check-prefix=CHECK-SUGAR
// RUN: %target-swift-ide-test -print-expr-type -source-filename %S/Inputs/ExprType.swift -swift-version 5 -canonicalize-type | %FileCheck %s -check-prefix=CHECK-CANON

// CHECK-SUGAR: func foo() -> Int { return <expr type:"Int">1</expr> }
// CHECK-SUGAR: func bar(f: Float) -> Float { return <expr type:"Float"><expr type:"(Float) -> Float">bar</expr>(f: <expr type:"Float">1</expr>)</expr> }
// CHECK-SUGAR: func fooP(_ p: P) { <expr type:"()"><expr type:"(any P) -> ()">fooP</expr>(<expr type:"any P">p</expr>)</expr> }
// CHECK-SUGAR: <expr type:"()"><expr type:"Int">_</expr> = <expr type:"Int"><expr type:"[C]">a</expr>.count</expr></expr>
// CHECK-SUGAR: <expr type:"()"><expr type:"String">_</expr> = <expr type:"String"><expr type:"Int"><expr type:"(Int) -> Int"><expr type:"Int"><expr type:"String"><expr type:"[C]">a</expr>.description</expr>.count</expr>.<expr type:"(Int) -> (Int) -> Int">advanced</expr></expr>(by: <expr type:"Int">1</expr>)</expr>.description</expr></expr>
// CHECK-SUGAR: <expr type:"()"><expr type:"Int?">_</expr> = <expr type:"Int?"><expr type:"Int"><expr type:"(Int) -> Int"><expr type:"Int"><expr type:"S"><expr type:"S?"><expr type:"[Int : S]">a</expr>[<expr type:"Int">2</expr>]</expr>?</expr>.val</expr>.<expr type:"(Int) -> (Int) -> Int">advanced</expr></expr>(by: <expr type:"Int">1</expr>)</expr>.byteSwapped</expr></expr>


// CHECK-SUGAR: return <expr type:"MyInt"><expr type:"MyInt">a</expr> <expr type:"(Int, Int) -> Int">+</expr> <expr type:"MyInt">b</expr></expr>
// CHECK-CANON: return <expr type:"Int"><expr type:"Int">a</expr> <expr type:"(Int, Int) -> Int">+</expr> <expr type:"Int">b</expr></expr>
