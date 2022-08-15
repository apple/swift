// This file should not have any syntax or type checker errors.
// RUN: %target-typecheck-verify-swift

// RUN: %target-swift-ide-test -print-types -source-filename %s -fully-qualified-types=false | %FileCheck %s -strict-whitespace
// RUN: %target-swift-ide-test -print-types -source-filename %s -fully-qualified-types=true | %FileCheck %s -check-prefix=FULL -strict-whitespace

typealias MyInt = Int
// CHECK: TypeAliasDecl '''MyInt''' MyInt.Type{{$}}
// FULL:  TypeAliasDecl '''MyInt''' swift_ide_test.MyInt.Type{{$}}

func testVariableTypes(_ param: Int, param2: inout Double) {
// CHECK: FuncDecl '''testVariableTypes''' (_ param: Int, param2: inout Double) -> (){{$}}
// FULL:  FuncDecl '''testVariableTypes''' (_ param: Swift.Int, param2: inout Swift.Double) -> (){{$}}

  var a1 = 42
// CHECK: VarDecl '''a1''' Int{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int{{$}}
// FULL:  VarDecl '''a1''' Swift.Int{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int{{$}}
  a1 = 17; _ = a1

  
  var a2 : Int = 42
// CHECK: VarDecl '''a2''' Int{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int{{$}}
// FULL:  VarDecl '''a2''' Swift.Int{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int{{$}}
  a2 = 17; _ = a2

  var a3 = Int16(42)
// CHECK: VarDecl '''a3''' Int16{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int16{{$}}
// FULL:  VarDecl '''a3''' Swift.Int16{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int16{{$}}
  a3 = 17; _ = a3


  var a4 = Int32(42)
// CHECK: VarDecl '''a4''' Int32{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int32{{$}}
// FULL:  VarDecl '''a4''' Swift.Int32{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int32{{$}}
  a4 = 17; _ = a4

  var a5 : Int64 = 42
// CHECK: VarDecl '''a5''' Int64{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int64{{$}}
// FULL:  VarDecl '''a5''' Swift.Int64{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int64{{$}}
  a5 = 17; _ = a5

  var typealias1 : MyInt = 42
// CHECK: VarDecl '''typealias1''' MyInt{{$}}
// CHECK:         IntegerLiteralExpr:[[@LINE-2]] '''42''' Int{{$}}
// FULL:  VarDecl '''typealias1''' swift_ide_test.MyInt{{$}}
// FULL:          IntegerLiteralExpr:[[@LINE-4]] '''42''' Swift.Int{{$}}
  _ = typealias1 ; typealias1 = 1

  var optional1 = Optional<Int>.none
// CHECK: VarDecl '''optional1''' Int?{{$}}
// FULL:  VarDecl '''optional1''' Swift.Int?{{$}}
  _ = optional1 ; optional1 = nil

  var optional2 = Optional<[Int]>.none
  _ = optional2 ; optional2 = nil
// CHECK: VarDecl '''optional2''' [Int]?{{$}}
// FULL:  VarDecl '''optional2''' [Swift.Int]?{{$}}
}

func testFuncType1() {}
// CHECK: FuncDecl '''testFuncType1''' () -> (){{$}}
// FULL:  FuncDecl '''testFuncType1''' () -> (){{$}}

func testFuncType2() -> () {}
// CHECK: FuncDecl '''testFuncType2''' () -> (){{$}}
// FULL:  FuncDecl '''testFuncType2''' () -> (){{$}}

func testFuncType3() -> Void {}
// CHECK: FuncDecl '''testFuncType3''' () -> Void{{$}}
// FULL:  FuncDecl '''testFuncType3''' () -> Swift.Void{{$}}

func testFuncType4() -> MyInt {}
// CHECK: FuncDecl '''testFuncType4''' () -> MyInt{{$}}
// FULL:  FuncDecl '''testFuncType4''' () -> swift_ide_test.MyInt{{$}}

func testFuncType5() -> (Int) {}
// CHECK: FuncDecl '''testFuncType5''' () -> Int{{$}}
// FULL:  FuncDecl '''testFuncType5''' () -> Swift.Int{{$}}

func testFuncType6() -> (Int, Int) {}
// CHECK: FuncDecl '''testFuncType6''' () -> (Int, Int){{$}}
// FULL:  FuncDecl '''testFuncType6''' () -> (Swift.Int, Swift.Int){{$}}

func testFuncType7(_ a: Int, withFloat b: Float) {}
// CHECK: FuncDecl '''testFuncType7''' (_ a: Int, withFloat: Float) -> (){{$}}
// FULL:  FuncDecl '''testFuncType7''' (_ a: Swift.Int, withFloat: Swift.Float) -> (){{$}}

func testVariadicFuncType(_ a: Int, b: Float...) {}
// CHECK: FuncDecl '''testVariadicFuncType''' (_ a: Int, b: Float...) -> (){{$}}
// FULL:  FuncDecl '''testVariadicFuncType''' (_ a: Swift.Int, b: Swift.Float...) -> (){{$}}

func testCurriedFuncType1(_ a: Int) -> (_ b: Float) -> () {}
// CHECK: FuncDecl '''testCurriedFuncType1''' (_ a: Int) -> (_ b: Float) -> (){{$}}
// FULL:  FuncDecl '''testCurriedFuncType1''' (_ a: Swift.Int) -> (_ b: Swift.Float) -> (){{$}}

protocol FooProtocol {}
protocol BarProtocol {}
protocol QuxProtocol { associatedtype Qux }

struct GenericStruct<A, B : FooProtocol> {}

func testInGenericFunc1<A, B : FooProtocol, C : FooProtocol & BarProtocol>(_ a: A, b: B, c: C) {
// CHECK: FuncDecl '''testInGenericFunc1''' <A, B, C where B : FooProtocol, C : BarProtocol, C : FooProtocol> (_ a: A, b: B, c: C) -> (){{$}}
// FULL:  FuncDecl '''testInGenericFunc1''' <A, B, C where B : swift_ide_test.FooProtocol, C : swift_ide_test.BarProtocol, C : swift_ide_test.FooProtocol> (_ a: A, b: B, c: C) -> (){{$}}

  var a1 = a
  _ = a1; a1 = a
// CHECK: VarDecl '''a1''' A{{$}}
// FULL:  VarDecl '''a1''' A{{$}}

  var b1 = b
  _ = b1; b1 = b
// CHECK: VarDecl '''b1''' B{{$}}
// FULL:  VarDecl '''b1''' B{{$}}

  var gs1 = GenericStruct<A, B>()
  _ = gs1; gs1 = GenericStruct<A, B>()
// CHECK: VarDecl '''gs1''' GenericStruct<A, B>{{$}}
// CHECK:    CallExpr:[[@LINE-2]] '''GenericStruct<A, B>()''' GenericStruct<A, B>{{$}}
// CHECK:          ConstructorRefCallExpr:[[@LINE-3]] '''GenericStruct<A, B>''' () -> GenericStruct<A, B>

// FULL:  VarDecl '''gs1''' swift_ide_test.GenericStruct<A, B>{{$}}
// FULL:    CallExpr:[[@LINE-6]] '''GenericStruct<A, B>()''' swift_ide_test.GenericStruct<A, B>{{$}}
// FULL:          ConstructorRefCallExpr:[[@LINE-7]] '''GenericStruct<A, B>''' () -> swift_ide_test.GenericStruct<A, B>
}

func testInGenericFunc2<T : QuxProtocol, U : QuxProtocol>(_: T, _: U) where T.Qux == U.Qux {}
// CHECK: FuncDecl '''testInGenericFunc2''' <T, U where T : QuxProtocol, U : QuxProtocol, T.Qux == U.Qux> (T, U) -> (){{$}}
// FULL:  FuncDecl '''testInGenericFunc2''' <T, U where T : swift_ide_test.QuxProtocol, U : swift_ide_test.QuxProtocol, T.Qux == U.Qux> (T, U) -> (){{$}}

