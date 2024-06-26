// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=swift-6)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++14)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++17)
// RUN: %target-run-simple-swift(-I %S/Inputs -cxx-interoperability-mode=upcoming-swift -Xcc -std=c++20)

// Also test this with a bridging header instead of the StdSet module.
// RUN: %empty-directory(%t2)
// RUN: cp %S/Inputs/std-set.h %t2/std-set-bridging-header.h
// RUN: %target-run-simple-swift(-D BRIDGING_HEADER -import-objc-header %t2/std-set-bridging-header.h -Xfrontend -enable-experimental-cxx-interop)
// RUN: %target-run-simple-swift(-D BRIDGING_HEADER -import-objc-header %t2/std-set-bridging-header.h -cxx-interoperability-mode=swift-6)
// RUN: %target-run-simple-swift(-D BRIDGING_HEADER -import-objc-header %t2/std-set-bridging-header.h -cxx-interoperability-mode=upcoming-swift)

// REQUIRES: executable_test

import StdlibUnittest
#if !BRIDGING_HEADER
import StdSet
#endif
import CxxStdlib
import Cxx

var StdSetTestSuite = TestSuite("StdSet")

StdSetTestSuite.test("iterate over Swift.Array") {
    let s = Array(initSetOfCInt())
    var result = [CInt]()
    for x in s {
        result.append(x)
    }
    expectEqual(result[0], 1)
    expectEqual(result[1], 3)
    expectEqual(result[2], 5)
}

StdSetTestSuite.test("SetOfCInt.contains") {
    // This relies on the `std::set` conformance to `CxxSet` protocol.
    let s = initSetOfCInt()
    expectTrue(s.contains(1))
    expectFalse(s.contains(2))
    expectTrue(s.contains(3))
}

StdSetTestSuite.test("UnorderedSetOfCInt.contains") {
    // This relies on the `std::unordered_set` conformance to `CxxSet` protocol.
    let s = initUnorderedSetOfCInt()
    expectFalse(s.contains(1))
    expectTrue(s.contains(2))
    expectFalse(s.contains(3))
}

StdSetTestSuite.test("MultisetOfCInt.contains") {
    // This relies on the `std::multiset` conformance to `CxxSet` protocol.
    let s = initMultisetOfCInt()
    expectFalse(s.contains(1))
    expectTrue(s.contains(2))
    expectFalse(s.contains(3))
}

StdSetTestSuite.test("SetOfCInt.init()") {
    let s = SetOfCInt([1, 3, 5])
    expectTrue(s.contains(1))
    expectFalse(s.contains(2))
    expectTrue(s.contains(3))
}

StdSetTestSuite.test("UnorderedSetOfCInt.init()") {
    let s = UnorderedSetOfCInt([1, 3, 5])
    expectTrue(s.contains(1))
    expectFalse(s.contains(2))
    expectTrue(s.contains(3))
}

StdSetTestSuite.test("SetOfCInt.insert") {
    var s = SetOfCInt()
    expectFalse(s.contains(123))

    let res1 = s.insert(123)
    expectTrue(res1.inserted)
    expectTrue(s.contains(123))

    let res2 = s.insert(123)
    expectFalse(res2.inserted)
    expectTrue(s.contains(123))
}

StdSetTestSuite.test("UnorderedSetOfCInt.insert") {
    var s = UnorderedSetOfCInt()
    expectFalse(s.contains(123))

    let res1 = s.insert(123)
    expectTrue(res1.inserted)
    expectTrue(s.contains(123))

    let res2 = s.insert(123)
    expectFalse(res2.inserted)
    expectTrue(s.contains(123))
}

StdSetTestSuite.test("SetOfCInt.erase") {
    var s = initSetOfCInt()
    expectTrue(s.contains(1))
    s.erase(1)
    expectFalse(s.contains(1))
    s.erase(1)
    expectFalse(s.contains(1))
}

StdSetTestSuite.test("UnorderedSetOfCInt.erase") {
    var s = initUnorderedSetOfCInt()
    expectTrue(s.contains(2))
    s.erase(2)
    expectFalse(s.contains(2))
    s.erase(2)
    expectFalse(s.contains(2))
}

runAllTests()
