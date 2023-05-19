// RUN: %target-swift-ide-test -reconstruct-type -source-filename %s | %FileCheck %s -implicit-check-not="FAILURE"

// This test doesn't test a whole lot now that the more general (but buggy)
// getDeclFromMangledSymbolName() has been replaced with getTypeDeclForMangling().

// However we're still printing and reconstructing types for all the expressions
// we see here, even though there are no check lines.

// More comprehensive tests for getType{,Decl}ForMangling() are found in
// test/TypeDecoder/.

struct Mystruct1 {
// CHECK: decl: struct Mystruct1
  func s1f1() -> Int { return 0 }
  var intField = 3
}
struct MyStruct2 {
// CHECK: decl: struct MyStruct2
  init() {}
  init(x: Int) {}
  init(x: Int, y: Int) {}
}

class Myclass1 {
// CHECK: decl: class Myclass1
  var intField = 4
}

func f1() {
  var s1ins = Mystruct1() // Implicit ctor
  _ = Mystruct1(intField: 1) // Implicit ctor

  s1ins.intField = 34
// CHECK: type: Mystruct1
// CHECK: type: Int

  var c1ins = Myclass1()
// CHECK: type: Myclass1

  c1ins.intField = 3
// CHECK: type: Int

  s1ins.s1f1()
// CHECK: type: Mystruct1
// CHECK: type: (Mystruct1) -> () -> Int

  if let ifletf1 = Int?(1) {
  }
}

class Myclass2 {
// CHECK: decl: class Myclass2
  func f1() {

    var arr1 = [1, 2]
// CHECK: type: [Int]

    arr1.append(1)
// CHECK: type: (inout Array<Int>) -> (__owned Int) -> ()

    var arr2 : [Mystruct1]
// CHECK: type: [Mystruct1]

    arr2.append(Mystruct1())
// CHECK: type: (inout Array<Mystruct1>) -> (__owned Mystruct1) -> ()

    var arr3 : [Myclass1]
// CHECK: type: [Myclass1]

    arr3.append(Myclass1())
// CHECK: type: (inout Array<Myclass1>) -> (__owned Myclass1) -> ()

    _ = Myclass2.init()
  }
}

// CHECK: decl: enum MyEnum
enum MyEnum {
  case ravioli
  case pasta

  func method() -> Int { return 0 }

  func compare(_ other: MyEnum) -> Int {
    return 0
  }

  mutating func mutatingMethod() {}
}

func f2() {
// CHECK: type: (MyEnum.Type) -> MyEnum
  var e = MyEnum.pasta

// CHECK: type: (MyEnum) -> () -> Int
  e.method()
// CHECK: (MyEnum) -> (MyEnum) -> Int
  e.compare(e)
// CHECK: (inout MyEnum) -> () -> ()
  e.mutatingMethod()
}

struct MyGenStruct1<T, U: ExpressibleByStringLiteral, V: Sequence> {
// CHECK: decl: struct MyGenStruct1<T, U, V> where U : ExpressibleByStringLiteral, V : Sequence

  let x: T
  let y: U
  let z: V

  func test000() {
    _ = x
// CHECK: type: T
    _ = y
// CHECK: type: U
    _ = z
// CHECK: type: V
  }

  func takesT(_ t: T) {
  }
}

let genstruct1 = MyGenStruct1<Int, String, [Float]>(x: 1, y: "", z: [1.0])

func test001() {
  _ = genstruct1
// CHECK: type: MyGenStruct1<Int, String, [Float]>

  var genstruct2: MyGenStruct1<Int, String, [Int: Int]>
  _ = genstruct2
// CHECK: type: MyGenStruct1<Int, String, [Int : Int]>
  _ = genstruct2.x
// CHECK: type: Int
  _ = genstruct2.y
// CHECK: type: String
  _ = genstruct2.z
// CHECK: type: [Int : Int]

  genstruct2.takesT(123)
}

// CHECK: decl: protocol P1
protocol P1 {}

func foo1(p: P1) {
// CHECK: type: (any P1) -> ()
  foo1(p: p)
}

// CHECK: decl: protocol P2
protocol P2 {}

func foo2(p: P1 & P2) {
  foo2(p: p)
}

func foo3(p: P1 & AnyObject) {
  foo3(p: p)
}

func foo4(p: P1 & P2 & Myclass1) {
  foo4(p: p)
}

func genericFunction<T : AnyObject>(t: T) {
  genericFunction(t: t)
}

func takesInOut(fn: (inout Int) -> ()) {}

struct Outer {
  struct Inner {
    let x: Int
  }

  struct GenericInner<T> {
    let t: T
  }
}

struct GenericOuter<T> {
  struct Inner {
    let t: T
    let x: Int
  }

  struct GenericInner<U> {
    let t: T
    let u: U
  }
}

func takesGeneric(_ t: Outer.GenericInner<Int>) {
  takesGeneric(t)
}

func takesGeneric(_ t: GenericOuter<Int>.Inner) {
  takesGeneric(t)
}

func takesGeneric(_ t: GenericOuter<Int>.GenericInner<String>) {
  takesGeneric(t)
}

func hasLocalDecls() {
  func localFunction() {}

  // CHECK: decl: struct LocalType  for 'LocalType' usr=s:14swift_ide_test13hasLocalDeclsyyF0E4TypeL_V
  struct LocalType {
    func localMethod() {}

    subscript(x: Int) -> Int { get {} set {} }
  }

  // CHECK: decl: class LocalClass for 'LocalClass' usr=s:14swift_ide_test13hasLocalDeclsyyF0E5ClassL_C
  class LocalClass {
    deinit {}
  }

  // CHECK: decl: typealias LocalAlias = LocalType for 'LocalAlias' usr=s:14swift_ide_test13hasLocalDeclsyyF0E5AliasL_a
  typealias LocalAlias = LocalType
}

fileprivate struct VeryPrivateData {}

fileprivate func privateFunction(_ d: VeryPrivateData) {}

struct HasSubscript {
  subscript(_ t: Int) -> Int {
    get {
      return t
    }
    set {}
  }
}

struct HasGenericSubscript<T> {
  subscript<U>(_ t: T) -> U {
    get {
      return t as! U
    }
    set {}
  }
}

private
func patatino<T: Comparable>(_ vers1: T, _ vers2: T) -> Bool {
  return vers1 < vers2;
}

@available(OSX 10.9, *)
@_originallyDefinedIn(module: "OtherModule", OSX 10.13)
public struct MovedHere {
  public func foo() {}
}
