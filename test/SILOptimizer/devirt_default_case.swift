
// RUN: %target-swift-frontend -enable-spec-devirt -O -module-name devirt_default_case -emit-sil %s | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-NORMAL %s
// RUN: %target-swift-frontend -enable-spec-devirt -O -module-name devirt_default_case -emit-sil -enable-testing %s | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-TESTABLE %s

@_silgen_name("action")
func action(_ n:Int) -> ()

// public class
open class Base1 {
  @inline(never) func inner() { action(1)}
  func middle() { inner() }
// Check that call to Base1.middle cannot be devirtualized
//
// CHECK-LABEL: sil @$s19devirt_default_case5Base1C5outer{{[_0-9a-zA-Z]*}}F
// CHECK: class_method 
// CHECK: } // end sil function '$s19devirt_default_case5Base1C5outer{{[_0-9a-zA-Z]*}}F'
  public func outer() { 
    middle() 
  }
}

// public class
open class Derived1 : Base1 {
  override func inner() { action(2) }
  @inline(never) final override func middle() { inner() }
}

// private class
private class Base2 {
  @inline(never) func inner() { action(3)}
  func middle() { inner() }
  func outer() { middle() }
}

// private class
private class Derived2 : Base2 {
  override func inner() { action(4) }
  @inline(never) final override func middle() { inner() }
}


// Check that call to Base2.middle can be devirtualized
//
// CHECK-LABEL: sil @$s19devirt_default_case9callOuteryS2iF
// CHECK: function_ref @$s19devirt_default_case5Base233_{{.*}}5inner
// CHECK: function_ref @$s19devirt_default_case8Derived233_{{.*}}6middle
// CHECK-NOT: class_method
// CHECK: } // end sil function '$s19devirt_default_case9callOuteryS2iF'
public func callOuter(_ x: Int) -> Int {

  var o:Base2
  
  if x == 1 {
    o = Base2()
  } else {
    o = Derived2()
  }
  
  o.outer()
  return x
}

// internal class
class Base3 {
  @inline(never) func inner() { action(5)}
  @inline(never) func middle() { inner() }
// Check that call to Base3.middle can be devirtualized when not compiling
// for testing.
//
// CHECK-LABEL: sil{{( hidden)?}} [noinline] @$s19devirt_default_case5Base3C5outeryyF : $@convention(method) (@guaranteed Base3) -> () {
// CHECK: function_ref @$s19devirt_default_case5Base3C6middleyyF
// CHECK: function_ref @$s19devirt_default_case8Derived333_{{.*}}6middle
// CHECK-NORMAL-NOT: class_method
// CHECK-TESTABLE: class_method %0 : $Base3, #Base3.middle
// CHECK: } // end sil function '$s19devirt_default_case5Base3C5outeryyF'
  @inline(never) func outer() {
    middle()
  }
}

// private class
private class Derived3 : Base3 {
  override func inner() { action(6) }
  @inline(never) final override func middle() { inner() }
  override func outer() {
  }
}

class A2 { @inline(never) func f() -> Int { return 0 } }
class B2 : A2 {}
class C2 : A2 {}
class D2: B2 {}
class E2 :C2 {}

class A3 { @inline(never) func f() -> Int { return 0 } }
class B3 : A3 { @inline(never) override func f() -> Int { return 1 }}
class C3 : A3 {}
class D3: C3 {}
class E3 :C3 {}

// CHECK-TESTABLE: sil{{( hidden)?}} [noinline] @$s19devirt_default_case3fooySiAA2A3CF

public func testfoo1() -> Int {
  return foo(E2())
}


public func testfoo3() -> Int {
  return foo(E3())
}

// Check that call to A3.f() can be devirtualized.
//
// CHECK-NORMAL: sil hidden [noinline] @$s19devirt_default_case3fooySiAA2A3CF
// CHECK-NORMAL: function_ref @$s19devirt_default_case2B3C1fSiyFTf4d_n
// CHECK-NORMAL: function_ref @$s19devirt_default_case2A3C1fSiyFTf4d_n
// CHECK-NORMAL-NOT: class_method
// CHECK: } // end sil function '$s19devirt_default_case3fooySiAA2A3CF'

class Base4 {
  @inline(never)
  func test() { 
// Check that call to foo() can be devirtualized
//
// CHECK-LABEL: sil{{( hidden)?}} [noinline] @$s19devirt_default_case5Base4C4testyyF
// CHECK: function_ref @$s19devirt_default_case5Base4C3fooyyFTf4d_n
// CHECK: function_ref @$s19devirt_default_case8Derived4C3fooyyFTf4d_n
// CHECK-NORMAL-NOT: class_method
// CHECK-TESTABLE: class_method %0 : $Base4, #Base4.foo
// CHECK: } // end sil function '$s19devirt_default_case5Base4C4testyyF'
    foo() 
  }
  
  @inline(never) func foo() { }
}


// A, C,D,E all use the same implementation. 
// B has its own implementation.
@inline(never)
func foo(_ a: A3) -> Int {
  return a.f()
}

class Derived4 : Base4 {
  @inline(never) override func foo() { }
}

open class Base5 {
  @inline(never)
  open func test() { 
    foo() 
  }
  
  @inline(never) public final func foo() { }
}

class Derived5 : Base5 {
}

open class C6 { 
  func bar() -> Int { return 1 } 
}

class D6 : C6 { 
  override func bar() -> Int { return 2 } 
}

@inline(never)
func check_static_class_devirt(_ c: C6) -> Int { 
// Check that C.bar() and D.bar() are devirtualized.
//
// CHECK-LABEL: sil{{( hidden)?}} [noinline] @$s19devirt_default_case019check_static_class_A0ySiAA2C6CF
// CHECK: checked_cast_br [exact] C6 in %0 : $C6 to C6
// CHECK: checked_cast_br [exact] C6 in %0 : $C6 to D6
// CHECK: class_method
// CHECK: } // end sil function '$s19devirt_default_case019check_static_class_A0ySiAA2C6CF'
  return c.bar() 
}

public func test_check_static_class_devirt() -> Int {
  return check_static_class_devirt(D6())
}


class A7 { @inline(never) func foo() -> Bool { return false } }
class B7 : A7 { @inline(never) override func foo() -> Bool { return true } }

public func test_check_call_on_downcasted_instance() -> Bool {
  return check_call_on_downcasted_instance(B7())
}

@inline(never)
func callIt(_ b3: Base3, _ b4: Base4, _ b5: Base5) {
  b3.outer()
  b4.test()
  b5.test()
}

public func externalEntryPoint() {
  callIt(Base3(), Base4(), Base5())
}

open class M {
  func foo() -> Int32 {
    return 0
  }
}


open class M1: M {
  @inline(never)
  override func foo() -> Int32 {
    return 1
  }
}

internal class M2: M1 {
  @inline(never)
  override func foo() -> Int32 {
    return 2
  }
}

internal class M3: M1 {
  @inline(never)
  override func foo() -> Int32 {
    return 3
  }
}

internal class M22: M2 {
  @inline(never)
  override func foo() -> Int32 {
    return 22
  }
}

internal class M222: M22 {
  @inline(never)
  override func foo() -> Int32 {
    return 222
  }
}

internal class M33: M3 {
  @inline(never)
  override func foo() -> Int32 {
    return 33
  }
}


// Check that speculative devirtualization tries to devirtualize the first N
// alternatives, if it has too many.
// The alternatives should be taken in a breadth-first order, starting with
// the static type of the instance.

@inline(never)
public func testSpeculativeDevirtualizationWithTooManyAlternatives(_ c:M1) -> Int32{
  return c.foo()
}


@inline(never)
func foo(_ a: A2) -> Int {
  return a.f()
}

@inline(never)
func check_call_on_downcasted_instance(_ a: A7) -> Bool {
  if a is B7 {
    return (a as! B7).foo()
  }
  return a.foo()
}
