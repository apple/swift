// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop -Xfrontend -validate-tbd-against-ir=none -Xfrontend -disable-llvm-verify -Xfrontend -disable-availability-checking -Onone -D NO_OPTIMIZATIONS)
// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop -Xfrontend -validate-tbd-against-ir=none -Xfrontend -disable-llvm-verify -Xfrontend -disable-availability-checking -O)
//
// REQUIRES: executable_test
// XFAIL: OS=windows-msvc

// Temporarily disable when running with an older runtime (rdar://128681137)
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest
import ReferenceCounted

var ReferenceCountedTestSuite = TestSuite("Foreign reference types that have reference counts")

@inline(never)
public func blackHole<T>(_ _: T) {  }

@inline(never)
func localTest() {
    var x = NS.LocalCount.create()
#if NO_OPTIMIZATIONS
    expectEqual(x.value, 8) // This is 8 because of "var x" "x.value" * 2, two method calls on x, and "(x, x, x)".
#endif

    expectEqual(x.returns42(), 42)
    expectEqual(x.constMethod(), 42)

    let t = (x, x, x)
#if NO_OPTIMIZATIONS
    expectEqual(x.value, 5)
#endif
}

ReferenceCountedTestSuite.test("Local") {
    expectNotEqual(finalLocalRefCount, 0)
    localTest()
    expectEqual(finalLocalRefCount, 0)
}

var globalOptional: NS.LocalCount? = nil

ReferenceCountedTestSuite.test("Global optional holding local ref count") {
    globalOptional = NS.LocalCount.create()
    expectEqual(finalLocalRefCount, 1)
}

@inline(never)
func globalTest1() {
    var x = GlobalCount.create()
    let t = (x, x, x)
#if NO_OPTIMIZATIONS
    expectEqual(globalCount, 4)
#endif
    blackHole(t)
}

@inline(never)
func globalTest2() {
    var x = GlobalCount.create()
#if NO_OPTIMIZATIONS
    expectEqual(globalCount, 1)
#endif
}

ReferenceCountedTestSuite.test("Global") {
    expectEqual(globalCount, 0)
    globalTest1()
    globalTest2()
    expectEqual(globalCount, 0)
}

var globalArray: [GlobalCount] = []

ReferenceCountedTestSuite.test("Global array") {
    expectEqual(globalCount, 0)

    globalArray = [GlobalCount.create()]
#if NO_OPTIMIZATIONS
    expectEqual(globalCount, 1)
#endif

    globalArray = []
    expectEqual(globalCount, 0)
}

runAllTests()
