// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all -enable-experimental-feature ReferenceBindings)

// REQUIRES: executable_test

import StdlibUnittest

defer { runAllTests() }

var tests = TestSuite("reference bindings")

var global: String = "globalName"
tests.test("global access") {
    expectEqual(global, "globalName")
    do {
      inout x = global
      expectEqual(x, "globalName")
    }
    expectEqual(global, "globalName")
}

tests.test("multiple global access exclusivity error")
  .crashOutputMatches("Simultaneous accesses to")
  .code {
    expectCrashLater()

    @inline(never)
    func test(_ x: inout String) {
        inout x = global
    }
    test(&global)
}

tests.test("Class Instance Field Access") {
    class Klass {
        var name: String = "klassName"
    }

    let k = Klass()
    expectEqual(k.name, "klassName")
    do {
        inout x = k.name
        expectEqual(x, "klassName")
    }
    expectEqual(k.name, "klassName")
}

tests.test("Var Access") {
    var varName = "varName"
    do {
        inout x = varName
        expectEqual(x, "varName")
    }
    expectEqual(varName, "varName")

    struct S {
        var s1 = "field1"
        var s2 = "field2"
    }

    var s = S()
    expectEqual(s.s1, "field1")
    expectEqual(s.s2, "field2")
    do {
        inout x = s.s1
        expectEqual(x, "field1")
    }
    expectEqual(s.s1, "field1")
    expectEqual(s.s2, "field2")
    do {
        inout x2 = s.s2
        expectEqual(x2, "field2")
    }
    expectEqual(s.s1, "field1")
    expectEqual(s.s2, "field2")
}


tests.test("InOut Access") {
    var varName = "varName"
    func inoutTest(_ inputX: inout String) {
        expectEqual(inputX, "varName")
        inout x = inputX
        expectEqual(x, "varName")
    }
    inoutTest(&varName)
    expectEqual(varName, "varName")
}
