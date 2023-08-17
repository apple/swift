// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// FIXME: also run in C++20 mode when conformance works properly on UBI platform (rdar://109366764):
// %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -std=gnu++20)
//
// REQUIRES: executable_test

import StdlibUnittest
import StdVector
import CxxStdlib

var StdVectorTestSuite = TestSuite("StdVector")

StdVectorTestSuite.test("VectorOfInt.init") {
    let v = Vector()
    expectEqual(v.size(), 0)
    expectTrue(v.empty())
}

StdVectorTestSuite.test("VectorOfInt.init(sequence)") {
    let v = Vector([])
    expectEqual(v.size(), 0)
    expectTrue(v.empty())

    let v2 = Vector([1, 2, 3])
    expectEqual(v2.size(), 3)
    expectFalse(v2.empty())
    expectEqual(v2[0], 1)
    expectEqual(v2[1], 2)
    expectEqual(v2[2], 3)
}

StdVectorTestSuite.test("VectorOfString.init(sequence)") {
    let v = VectorOfString([])
    expectEqual(v.size(), 0)
    expectTrue(v.empty())

    let v2 = VectorOfString(["", "ab", "abc"])
    expectEqual(v2.size(), 3)
    expectFalse(v2.empty())
    expectEqual(v2[0], "")
    expectEqual(v2[1], "ab")
    expectEqual(v2[2], "abc")

    let first = takesVectorOfString(["abc", "qwe"])
    expectEqual(first, "abc")
}

StdVectorTestSuite.test("VectorOfInt as ExpressibleByArrayLiteral") {
    let v: Vector = []
    expectEqual(v.size(), 0)
    expectTrue(v.empty())

    let v2: Vector = [1, 2, 3]
    expectEqual(v2.size(), 3)
    expectFalse(v2.empty())
    expectEqual(v2[0], 1)
    expectEqual(v2[1], 2)
    expectEqual(v2[2], 3)
}

StdVectorTestSuite.test("VectorOfInt.push_back") {
    var v = Vector()
    let _42: CInt = 42
    v.push_back(_42)
    expectEqual(v.size(), 1)
    expectFalse(v.empty())
    expectEqual(v[0], 42)
}

func fill(vector v: inout Vector) {
    v.push_back(1)
    v.push_back(2)
    v.push_back(CInt(3))
}

StdVectorTestSuite.test("VectorOfInt for loop") {
    var v = Vector()
    fill(vector: &v)

    var count: CInt = 1
    for e in v {
        expectEqual(e, count)
        count += 1
    }
    expectEqual(count, 4)
}

StdVectorTestSuite.test("VectorOfString for loop") {
    var v = VectorOfString()
    var count = 0
    for _ in v {
        count += 1
    }
    expectEqual(count, 0)

    v.push_back(std.string("abc"))
    v.push_back(std.string("ab"))
    for it in v {
        count += it.length()
    }
    expectEqual(count, 5)
}

StdVectorTestSuite.test("VectorOfInt.map") {
    var v = Vector()
    fill(vector: &v)

    let a = v.map { $0 + 5 }
    expectEqual(a, [6, 7, 8])
}

StdVectorTestSuite.test("VectorOfString.map") {
    var v = VectorOfString()
    v.push_back(std.string("abc"))
    v.push_back(std.string("a"))
    v.push_back(std.string("ab"))

    let a = v.map { $0.length() }
    expectEqual(a, [3, 1, 2])
}

runAllTests()
