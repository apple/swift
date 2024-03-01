// REQUIRES: executable_test
// RUN: %target-run-simple-swift(-enable-experimental-feature TrailingComma)
// RUN:

import StdlibUnittest

var suite = TestSuite("TrailingCommaTestSuite")

suite.test("tuple") {

    let tuple = (a: 1, b: 2, c: 3,)

    expectEqual(tuple.a, 1)
    expectEqual(tuple.b, 2)
    expectEqual(tuple.c, 3)

}

suite.test("argument list") { 

    func test(a: Int = 0, b: Int = 0, c: Int = 0,) -> Int {
        return a + b + c
    }
    
    let _ = test(a: 1, b: 2, c: 3,)
    
    let _ = test(
        a: 1, 
        b: 2, 
        // c: 3,
    )

}

suite.test("if conditions") { 

    func f(_ block: (Bool) -> Bool) -> Bool { block(true) }

    var value = 0

    if true, { value = 1 }

    expectEqual(value, 1)

    if true, f { $0 }, { true }(), { value = 2 } else { value = 0 }
    
    expectEqual(value, 2)

    if true, {true}(), { value = 3 }
    
    expectEqual(value, 3)

    if true, { a in a == 1 }(1), { value = 4 } else { value = 5 }

    expectEqual(value, 4)

    if false, {true}(), { value = 4 } else if true, {true}(), { value = 5 } else { value = 6 }

    expectEqual(value, 5)

    if false, {true}(), { value = 4 } else if false, {true}(), { value = 5 } else { value = 6 }
    
    expectEqual(value, 6)

}

suite.test("guard conditions") { 

    func string(value: Int?) -> String {
        guard true, {true}(), { bool in bool }(true), let value, else {
            return "nil"
        }
        return "\(value)"
    }

    expectEqual(string(value: nil), "nil")
    expectEqual(string(value: 1), "1")

}

suite.test("while conditions") { 

    var value = 5

    while value != 0, {
        value -= 1
    }

    expectEqual(value, 0)

}

runAllTests()