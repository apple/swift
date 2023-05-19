// RUN: %target-swift-ide-test -print-expr-type -fully-qualified-types -source-filename %S/Inputs/ExprType.swift -swift-version 5 | %FileCheck %s -check-prefix=CHECK-SUGAR
// RUN: %target-swift-ide-test -print-expr-type -fully-qualified-types -source-filename %S/Inputs/ExprType.swift -swift-version 5 -canonicalize-type | %FileCheck %s -check-prefix=CHECK-CANON

// CHECK-SUGAR: func foo() -> Int { return <expr type:"Swift.Int">1</expr> }
// CHECK-SUGAR: func bar(f: Float) -> Float { return <expr type:"Swift.Float"><expr type:"(Swift.Float) -> Swift.Float">bar</expr>(f: <expr type:"Swift.Float">1</expr>)</expr> }
// CHECK-SUGAR: func fooP(_ p: P) { <expr type:"()"><expr type:"(any swift_ide_test.P) -> ()">fooP</expr>(<expr type:"any swift_ide_test.P">p</expr>)</expr> }
// CHECK-SUGAR: <expr type:"()"><expr type:"Swift.Int">_</expr> = <expr type:"Swift.Int"><expr type:"[swift_ide_test.C]">a</expr>.count</expr></expr>
// CHECK-SUGAR: <expr type:"()"><expr type:"Swift.String">_</expr> = <expr type:"Swift.String"><expr type:"Swift.Int"><expr type:"(Swift.Int) -> Swift.Int"><expr type:"Swift.Int"><expr type:"Swift.String"><expr type:"[swift_ide_test.C]">a</expr>.description</expr>.count</expr>.<expr type:"(Swift.Int) -> (Swift.Int) -> Swift.Int">advanced</expr></expr>(by: <expr type:"Swift.Int">1</expr>)</expr>.description</expr></expr>
// CHECK-SUGAR: <expr type:"()"><expr type:"Swift.Int?">_</expr> = <expr type:"Swift.Int?"><expr type:"Swift.Int"><expr type:"(Swift.Int) -> Swift.Int"><expr type:"Swift.Int"><expr type:"swift_ide_test.S"><expr type:"swift_ide_test.S?"><expr type:"[Swift.Int : swift_ide_test.S]">a</expr>[<expr type:"Swift.Int">2</expr>]</expr>?</expr>.val</expr>.<expr type:"(Swift.Int) -> (Swift.Int) -> Swift.Int">advanced</expr></expr>(by: <expr type:"Swift.Int">1</expr>)</expr>.byteSwapped</expr></expr>


// CHECK-SUGAR: return <expr type:"swift_ide_test.MyInt"><expr type:"swift_ide_test.MyInt">a</expr> <expr type:"(Swift.Int, Swift.Int) -> Swift.Int">+</expr> <expr type:"swift_ide_test.MyInt">b</expr></expr>
// CHECK-CANON: return <expr type:"Swift.Int"><expr type:"Swift.Int">a</expr> <expr type:"(Swift.Int, Swift.Int) -> Swift.Int">+</expr> <expr type:"Swift.Int">b</expr></expr>
