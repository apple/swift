// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xfrontend -cxx-interop-getters-setters-as-properties)
//
// REQUIRES: executable_test

import StdlibUnittest
import ImplicitComputedProperties

var ImplicitComputedPropertiesTestSuite = TestSuite("ImplicitComputedProperties")

ImplicitComputedPropertiesTestSuite.test("LongNameAllLower") {
    var a = LongNameAllLower()
    expectEqual(a.foo, 42)
    a.foo = 32
    expectEqual(a.foo, 32)
}

ImplicitComputedPropertiesTestSuite.test("LongNameAllUpper") {
    var b = LongNameAllUpper()
    expectEqual(b.foo, 42)
    b.foo = 32
    expectEqual(b.foo, 32)
}

ImplicitComputedPropertiesTestSuite.test("UpperCaseMix") {
    var c = UpperCaseMix()
    expectEqual(c.foo, 42)
    c.foo = 32
    expectEqual(c.foo, 32)
}

ImplicitComputedPropertiesTestSuite.test("GetterOnly") {
    let d = GetterOnly()
    expectEqual(d.foo, 42)
}

ImplicitComputedPropertiesTestSuite.test("UTF8Str") {
    var e  = UTF8Str()
    expectEqual(e.utf8Str, 42)
    e.utf8Str = 32
    expectEqual(e.utf8Str, 32)
}

ImplicitComputedPropertiesTestSuite.test("setters") {
    var f = IntGetterSetter()
    expectEqual(f.x, 42)
    f.x = 1
    expectEqual(f.x, 1)
}

ImplicitComputedPropertiesTestSuite.test("UpperCaseGetterSetter") {
    var g = UpperCaseGetterSetter()
    expectEqual(g.foo, 42)
    g.foo = 32
    expectEqual(g.foo, 32)
}

// rdar://89453106 (We need to handle imported properties that return a reference)
//ImplicitComputedPropertiesTestSuite.test("ref") {
//    var Object = RefGetterSetter()
//    var i: CInt = 2
//    withUnsafePointer(to: &i) { Object.x = $0 }
//    expectEqual(Object.x.pointee, 2)
//}

ImplicitComputedPropertiesTestSuite.test("ptr") {
    var Object = PtrGetterSetter()
    var i: CInt = 2
    withUnsafeMutablePointer(to: &i) { Object.x! = $0 }
    expectEqual(Object.x!.pointee, 2)
}

ImplicitComputedPropertiesTestSuite.test("non trivial") {
    var Object = NonTrivialGetterSetter()
    expectEqual(Object.x.value, 42)
    Object.x = NonTrivial(value: 20)
    expectEqual(Object.x.value, 20)
}

ImplicitComputedPropertiesTestSuite.test("SnakeCaseGetterSetter") {
    var object = SnakeCaseGetterSetter()
    expectEqual(object.foo, 42)
    object.foo = 32
    expectEqual(object.foo, 32)
}

ImplicitComputedPropertiesTestSuite.test("SnakeCaseUTF8Str") {
    var object = SnakeCaseUTF8Str()
    expectEqual(object.utf8String, 42)
    object.utf8String = 32
    expectEqual(object.utf8String, 32)
}

runAllTests()