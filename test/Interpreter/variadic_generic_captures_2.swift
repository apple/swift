// RUN: %target-run-simple-swift(-Xfrontend -disable-concrete-type-metadata-mangled-name-accessors)
// RUN: %target-run-simple-swift

// REQUIRES: executable_test

import StdlibUnittest

var captures = TestSuite("VariadicGenericCaptures")

func f1<T>(_ fn: @autoclosure () -> T) {
  _ = fn()
}

func f2<T>(_ fn: @autoclosure @escaping () -> T) {
  _ = fn()
}

func f3<T>(_ fn: () -> T) {
  _ = fn()
}

func f4<T>(_ fn: @escaping () -> T) {
  _ = fn()
}

func f5<T: AnyObject>(_ fn: @autoclosure () -> T) {
  _ = fn()
}

func f6<T: AnyObject>(_ fn: @autoclosure @escaping () -> T) {
  _ = fn()
}

func f7<T: AnyObject>(_ fn: () -> T) {
  _ = fn()
}

func f8<T: AnyObject>(_ fn: @escaping () -> T) {
  _ = fn()
}

func g1<each T>(_ t: repeat each T) {
  repeat f1(each t)
  repeat f2(each t)
  repeat f3 { each t }
  repeat f4 { each t }
  repeat _ = ({ each t }())
  repeat _ = ({ (each t, each t) }())
}

func g2<each T: AnyObject>(_ t: repeat each T) {
  repeat f5(each t)
  repeat f6(each t)
  repeat f7 { each t }
  repeat f8 { each t }
  repeat _ = ({ (each t, each t) }())
}

class C {
  var x = LifetimeTracked(0)
}

class D: C {}

class E: D {}

captures.test("Test") {
  g1(C(), D(), E())
  g2(C(), D(), E())
}

runAllTests()