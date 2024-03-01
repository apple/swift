// RUN: %target-typecheck-verify-swift -D CONDITION_1

postfix operator ++
postfix func ++ (_: Int) -> Int { 0 }

struct OneResult {}
struct TwoResult {}

protocol MyProto {
    func optionalMethod() -> [Int]?
}
struct MyStruct {
    var optionalMember: MyProto? { nil }
    func methodOne() -> OneResult { OneResult() }
    func methodTwo() -> TwoResult { TwoResult() }
}

func globalFunc<T>(_ arg: T) -> T { arg }

func testBasic(baseExpr: MyStruct) {
    baseExpr
#if CONDITION_1
      .methodOne() // expected-warning {{result of call to 'methodOne()' is unused}}
#else
      .methodTwo()
#endif
}

MyStruct()
#if CONDITION_1
  .methodOne() // expected-warning {{result of call to 'methodOne()' is unused}}
#else
  .methodTwo()
#endif


func testInvalidContent(baseExpr: MyStruct, otherExpr: Int) {
  baseExpr      // expected-warning {{expression of type 'MyStruct' is unused}}
#if CONDITION_1
    { $0 + 1  } // expected-error {{closure expression is unused}}
#endif

  baseExpr      // expected-warning {{expression of type 'MyStruct' is unused}}
#if CONDITION_1
    + otherExpr // expected-error {{unary operator cannot be separated from its operand}}
                // expected-warning@-1 {{result of operator '+' is unused}}
#endif

  baseExpr
#if CONDITION_1
    .methodOne() // expected-warning {{result of call to 'methodOne()' is unused}}

  print("debug") // expected-error {{unexpected tokens in '#if' expression body}}
#endif
}

func testExprKind(baseExpr: MyStruct, idx: Int) {
  baseExpr
#if CONDITION_1
  .optionalMember?.optionalMethod()![idx]++ // expected-warning {{result of operator '++' is unused}}
#else
  .otherMethod(arg) {
    //...
  }
#endif

  baseExpr
#if CONDITION_1
  .methodOne() + 12 // expected-error {{unexpected tokens in '#if' expression body}}
                    // expected-warning@-1 {{result of call to 'methodOne()' is unused}}
#endif
}

func emptyElse(baseExpr: MyStruct) {
  baseExpr
#if CONDITION_1
    .methodOne() // expected-warning {{result of call to 'methodOne()' is unused}}
#elseif CONDITION_2
    // OK. Do nothing.
#endif

  baseExpr
#if CONDITION_1
    .methodOne() // expected-warning {{result of call to 'methodOne()' is unused}}
#elseif CONDITION_2
  return         // expected-error {{unexpected tokens in '#if' expression body}}
#endif
}

func consecutiveIfConfig(baseExpr: MyStruct) {
    baseExpr
#if CONDITION_1
  .methodOne()
#endif
#if CONDITION_2
  .methodTwo()
#endif
  .unknownMethod() // expected-error {{value of type 'OneResult' has no member 'unknownMethod'}}
}

func nestedIfConfig(baseExpr: MyStruct) {
  baseExpr
#if CONDITION_1
  #if CONDITION_2
    .methodOne()
  #endif
  #if CONDITION_1
    .methodTwo() // expected-warning {{result of call to 'methodTwo()' is unused}}
  #endif
#else
  .unknownMethod1()
  #if CONDITION_2
    .unknownMethod2()
  #endif
#endif
}

func ifconfigExprInExpr(baseExpr: MyStruct) {
  globalFunc( // expected-warning {{result of call to 'globalFunc' is unused}}
    baseExpr
#if CONDITION_1
      .methodOne()
#else
      .methodTwo()
#endif
  )
}

func canImportVersioned() {
#if canImport(A, _version: 2)
  let a = 1
#endif

#if canImport(A, _version: 2.2)
  let a = 1
#endif
  
#if canImport(A, _version: 2.2.2)
  let a = 1
#endif
  
#if canImport(A, _version: 2.2.2.2)
  let a = 1
#endif
  
#if canImport(A, _version: 2.2.2.2.2) // expected-warning {{trailing components of version '2.2.2.2' are ignored}}
  let a = 1
#endif

#if canImport(A, _underlyingVersion: 4)
  let a = 1
#endif

#if canImport(A, _underlyingVersion: 2.200)
  let a = 1
#endif
  
#if canImport(A, _underlyingVersion: 2.200.1)
  let a = 1
#endif
  
#if canImport(A, _underlyingVersion: 2.200.1.3)
  let a = 1
#endif

#if canImport(A, unknown: 2.2) // expected-error {{2nd parameter of canImport should be labeled as _version or _underlyingVersion}}
  let a = 1
#endif

#if canImport(A,)
  let a = 1
#endif
  
#if canImport(A, 2.2) // expected-error {{2nd parameter of canImport should be labeled as _version or _underlyingVersion}}
  let a = 1
#endif

#if canImport(A, 2.2, 1.1) // expected-error {{canImport can take only two parameters}}
  let a = 1
#endif
  
#if canImport(A, _version:) // expected-error {{expected expression in list of expressions}}
  let a = 1
#endif

#if canImport(A, _version: "") // expected-error {{_version argument cannot be empty}}
  let a = 1
#endif
  
#if canImport(A, _version: >=2.2) // expected-error {{cannot parse module version '>=2.2'}}
  let a = 1
#endif
  
#if canImport(A, _version: 20A301) // expected-error {{'A' is not a valid digit in integer literal}}
  let a = 1
#endif

#if canImport(A, _version: "20A301") // expected-error {{cannot parse module version '20A301'}}
  let a = 1
#endif
}
