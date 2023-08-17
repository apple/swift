
// RUN: %target-swift-emit-silgen -module-name default_arguments -Xllvm -sil-full-demangle -swift-version 4 %s | %FileCheck %s
// RUN: %target-swift-emit-silgen -module-name default_arguments -Xllvm -sil-full-demangle -swift-version 4 %s | %FileCheck %s --check-prefix=NEGATIVE

// __FUNCTION__ used as top-level parameter produces the module name.
// CHECK-LABEL: sil [ossa] @main
// CHECK:         string_literal utf8 "default_arguments"

// Test at top level.
testMagicLiterals()
closure { testMagicLiterals() }
autoclosure(testMagicLiterals())

// CHECK: string_literal utf8 "default_arguments"
let y : String = #function

// Default argument for first parameter.
// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments7defarg11i1d1sySi_SdSStFfA_ : $@convention(thin) () -> Int
// CHECK: [[LIT:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 17
// CHECK: [[INT:%[0-9]+]] = metatype $@thin Int.Type
// CHECK: [[CVT:%[0-9]+]] = function_ref @$sSi22_builtinIntegerLiteralSiBI_tcfC
// CHECK: [[RESULT:%[0-9]+]] = apply [[CVT]]([[LIT]], [[INT]]) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK: return [[RESULT]] : $Int

// Default argument for third parameter.
// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments7defarg11i1d1sySi_SdSStFfA1_ : $@convention(thin) () -> @owned String
// CHECK: [[LIT:%[0-9]+]] = string_literal utf8 "Hello"
// CHECK: [[LEN:%[0-9]+]] = integer_literal $Builtin.Word, 5
// CHECK: [[STRING:%[0-9]+]] = metatype $@thin String.Type
// CHECK: [[CVT:%[0-9]+]] = function_ref @$sSS21_builtinStringLiteral17utf8CodeUnitCount7isASCIISSBp_BwBi1_tcfC
// CHECK: [[RESULT:%[0-9]+]] = apply [[CVT]]([[LIT]], [[LEN]], {{[^,]+}}, [[STRING]]) : $@convention(method)
// CHECK: return [[RESULT]] : $String
func defarg1(i: Int = 17, d: Double, s: String = "Hello") { }

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments15testDefaultArg1yyF
func testDefaultArg1() {
  // CHECK: [[FLOATLIT:%[0-9]+]] = float_literal $Builtin.FPIEEE{{64|80}}, {{0x4009000000000000|0x4000C800000000000000}}
  // CHECK: [[FLOAT64:%[0-9]+]] = metatype $@thin Double.Type
  // CHECK: [[LITFN:%[0-9]+]] = function_ref @$sSd20_builtinFloatLiteralSdBf{{[_0-9]*}}__tcfC
  // CHECK: [[FLOATVAL:%[0-9]+]] = apply [[LITFN]]([[FLOATLIT]], [[FLOAT64]])
  // CHECK: [[DEF0FN:%[0-9]+]] = function_ref @$s17default_arguments7defarg1{{.*}}A_
  // CHECK: [[DEF0:%[0-9]+]] = apply [[DEF0FN]]()
  // CHECK: [[DEF2FN:%[0-9]+]] = function_ref @$s17default_arguments7defarg1{{.*}}A1_
  // CHECK: [[DEF2:%[0-9]+]] = apply [[DEF2FN]]()
  // CHECK: [[FNREF:%[0-9]+]] = function_ref @$s17default_arguments7defarg1{{[_0-9a-zA-Z]*}}F
  // CHECK: apply [[FNREF]]([[DEF0]], [[FLOATVAL]], [[DEF2]])
  defarg1(d:3.125)
}

func defarg2(_ i: Int, d: Double = 3.125, s: String = "Hello") { }

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments15testDefaultArg2{{[_0-9a-zA-Z]*}}F
func testDefaultArg2() {
// CHECK:  [[INTLIT:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 5
// CHECK:  [[INT64:%[0-9]+]] = metatype $@thin Int.Type
// CHECK:  [[LITFN:%[0-9]+]] = function_ref @$sSi22_builtinIntegerLiteralSiBI_tcfC
// CHECK:  [[I:%[0-9]+]] = apply [[LITFN]]([[INTLIT]], [[INT64]]) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK:  [[DFN:%[0-9]+]] = function_ref @$s17default_arguments7defarg2{{.*}}A0_ : $@convention(thin) () -> Double
// CHECK:  [[D:%[0-9]+]] = apply [[DFN]]() : $@convention(thin) () -> Double
// CHECK:  [[SFN:%[0-9]+]] = function_ref @$s17default_arguments7defarg2{{.*}}A1_ : $@convention(thin) () -> @owned String
// CHECK:  [[S:%[0-9]+]] = apply [[SFN]]() : $@convention(thin) () -> @owned String
// CHECK:  [[FNREF:%[0-9]+]] = function_ref @$s17default_arguments7defarg2{{[_0-9a-zA-Z]*}}F : $@convention(thin) (Int, Double, @guaranteed String) -> ()
// CHECK:  apply [[FNREF]]([[I]], [[D]], [[S]]) : $@convention(thin) (Int, Double, @guaranteed String) -> ()
  defarg2(5)
}

func autocloseFile(x: @autoclosure () -> String = #file,
                   y: @autoclosure () -> Int = #line) { }
// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments17testAutocloseFileyyF
func testAutocloseFile() {
  // CHECK-LABEL: sil private [transparent] [ossa] @$s17default_arguments17testAutocloseFileyyFSSyXEfu_ : $@convention(thin) () -> @owned String
  // CHECK: string_literal utf8{{.*}}default_arguments.swift

  // CHECK-LABEL: sil private [transparent] [ossa] @$s17default_arguments17testAutocloseFileyyFSiyXEfu0_ : $@convention(thin) () -> Int
  // CHECK: integer_literal $Builtin.IntLiteral, [[@LINE+1]]
  autocloseFile()
}

func testMagicLiterals(file: String = #file,
                       function: String = #function,
                       line: Int = #line,
                       column: Int = #column) {}

// Check that default argument generator functions don't leak information about
// user's source.
//
// NEGATIVE-NOT: sil hidden [ossa] @$s17default_arguments17testMagicLiteralsySS4file_SS8functionSi4lineSi6columntFfA_
//
// NEGATIVE-NOT: sil hidden [ossa] @$s17default_arguments17testMagicLiteralsySS4file_SS8functionSi4lineSi6columntFfA0_
//
// NEGATIVE-NOT: sil hidden [ossa] @$s17default_arguments17testMagicLiteralsySS4file_SS8functionSi4lineSi6columntFfA1_
//
// NEGATIVE-NOT: sil hidden [ossa] @$s17default_arguments17testMagicLiteralsySS4file_SS8functionSi4lineSi6columntFfA2_

// https://github.com/apple/swift/issues/54034

func genericMagicLiteral<T : ExpressibleByIntegerLiteral>(_ x: T = #column) -> T { x }

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments23testGenericMagicLiteralyyF
func testGenericMagicLiteral() {
  // CHECK:      [[RET:%[0-9]+]] = alloc_stack $Int
  // CHECK-NEXT: [[RAWLIT:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 35
  // CHECK-NEXT: [[INTTY:%[0-9]+]] = metatype $@thin Int.Type
  // CHECK-NEXT: // function_ref Swift.Int.init(_builtinIntegerLiteral: Builtin.IntLiteral) -> Swift.Int
  // CHECK-NEXT: [[LITFN:%[0-9]+]] = function_ref @$sSi22_builtinIntegerLiteralSiBI_tcfC
  // CHECK-NEXT: [[LIT:%[0-9]+]] = apply [[LITFN]]([[RAWLIT]], [[INTTY]]) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
  // CHECK-NEXT: [[LITARG:%[0-9]+]] = alloc_stack $Int
  // CHECK-NEXT: store [[LIT]] to [trivial] [[LITARG]] : $*Int
  // CHECK-NEXT: // function_ref default_arguments.genericMagicLiteral<A where A: Swift.ExpressibleByIntegerLiteral>(A) -> A
  // CHECK-NEXT: [[FN:%[0-9]+]] = function_ref @$s17default_arguments19genericMagicLiteralyxxs020ExpressibleByIntegerE0RzlF : $@convention(thin) <τ_0_0 where τ_0_0 : ExpressibleByIntegerLiteral> (@in_guaranteed τ_0_0) -> @out τ_0_0
  // CHECK-NEXT: apply [[FN]]<Int>([[RET]], [[LITARG]]) : $@convention(thin) <τ_0_0 where τ_0_0 : ExpressibleByIntegerLiteral> (@in_guaranteed τ_0_0) -> @out τ_0_0
  let _: Int = genericMagicLiteral()
}

func closure(_: () -> ()) {}
func autoclosure(_: @autoclosure () -> ()) {}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments25testCallWithMagicLiteralsyyF
// CHECK:         string_literal utf8 "testCallWithMagicLiterals()"
// CHECK:         string_literal utf8 "testCallWithMagicLiterals()"
// CHECK-LABEL: sil private [ossa] @$s17default_arguments25testCallWithMagicLiteralsyyFyyXEfU_
// CHECK:         string_literal utf8 "testCallWithMagicLiterals()"
// CHECK-LABEL: sil private [transparent] [ossa] @$s17default_arguments25testCallWithMagicLiteralsyyFyyXEfu_
// CHECK:         string_literal utf8 "testCallWithMagicLiterals()"
func testCallWithMagicLiterals() {
  testMagicLiterals()
  testMagicLiterals()
  closure { testMagicLiterals() }
  autoclosure(testMagicLiterals())
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments25testPropWithMagicLiteralsSivg
// CHECK:         string_literal utf8 "testPropWithMagicLiterals"
var testPropWithMagicLiterals: Int {
  testMagicLiterals()
  closure { testMagicLiterals() }
  autoclosure(testMagicLiterals())
  return 0
}

class Foo {

  // CHECK-LABEL: sil hidden [ossa] @$s17default_arguments3FooC3int6stringACSi_SStcfc : $@convention(method) (Int, @owned String, @owned Foo) -> @owned Foo
  // CHECK:         string_literal utf8 "init(int:string:)"
  init(int: Int, string: String = #function) {
    testMagicLiterals()
    closure { testMagicLiterals() }
    autoclosure(testMagicLiterals())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s17default_arguments3FooCfd
  // CHECK:         string_literal utf8 "deinit"
  deinit {
    testMagicLiterals()
    closure { testMagicLiterals() }
    autoclosure(testMagicLiterals())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s17default_arguments3FooCyS2icig
  // CHECK:         string_literal utf8 "subscript(_:)"
  subscript(x: Int) -> Int {
    testMagicLiterals()
    closure { testMagicLiterals() }
    autoclosure(testMagicLiterals())
    return x
  }
 
  // CHECK-LABEL: sil private [global_init_once_fn] [ossa] @{{.*}}WZ
  // CHECK:         string_literal utf8 "Foo" 
  static let x = Foo(int:0)

}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments16testSelectorCall_17withMagicLiteralsySi_SitF
// CHECK:         string_literal utf8 "testSelectorCall(_:withMagicLiterals:)"
func testSelectorCall(_ x: Int, withMagicLiterals y: Int) {
  testMagicLiterals()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments32testSelectorCallWithUnnamedPieceyySi_SitF
// CHECK:         string_literal utf8 "testSelectorCallWithUnnamedPiece(_:_:)"
func testSelectorCallWithUnnamedPiece(_ x: Int, _ y: Int) {
  testMagicLiterals()
}

// Test default arguments in an inherited subobject initializer
class SuperDefArg {
  init(int i: Int = 10) { }
}

// CHECK: sil hidden [ossa] @$s17default_arguments11SuperDefArgC3intACSi_tcfcfA_ : $@convention(thin) () -> Int

// CHECK-NOT: sil hidden [ossa] @$s17default_arguments9SubDefArgCAC3intSi_tcfcfA_ : $@convention(thin) () -> Int

class SubDefArg : SuperDefArg { }

// CHECK: sil hidden [ossa] @$s17default_arguments13testSubDefArgAA0deF0CyF : $@convention(thin) () -> @owned SubDefArg
func testSubDefArg() -> SubDefArg {
  // CHECK: function_ref @$s17default_arguments11SuperDefArgC3intACSi_tcfcfA_
  // CHECK: function_ref @$s17default_arguments9SubDefArgC{{[_0-9a-zA-Z]*}}fC
  // CHECK: return
  return SubDefArg()
}

// CHECK-NOT: sil hidden [ossa] @$s17default_arguments9SubDefArgCACSi3int_tcfcfA_ : $@convention(thin) () -> Int

// <rdar://problem/17379550>
func takeDefaultArgUnnamed(_ x: Int = 5) { }

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments25testTakeDefaultArgUnnamed{{[_0-9a-zA-Z]*}}F
func testTakeDefaultArgUnnamed(_ i: Int) {
  // CHECK: bb0([[I:%[0-9]+]] : $Int):
  // CHECK:   [[FN:%[0-9]+]] = function_ref @$s17default_arguments21takeDefaultArgUnnamedyySiF : $@convention(thin) (Int) -> ()
  // CHECK:   apply [[FN]]([[I]]) : $@convention(thin) (Int) -> ()
  takeDefaultArgUnnamed(i)
}

func takeDSOHandle(_ handle: UnsafeRawPointer = #dsohandle) { }

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments13testDSOHandleyyF
func testDSOHandle() {
  // CHECK: [[DSO_HANDLE:%[0-9]+]] = global_addr {{@__dso_handle|@__ImageBase}} : $*Builtin.RawPointer
  takeDSOHandle()
}

// Test __FUNCTION__ in an extension initializer. rdar://problem/19792181
extension SuperDefArg {
  static let extensionInitializerWithClosure: Int = { return 22 }()
}


// <rdar://problem/19086357> SILGen crashes reabstracting default argument closure in members
class ReabstractDefaultArgument<T> {
  init(a: (T, T) -> Bool = { _, _ in true }) {
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments32testDefaultArgumentReabstractionyyF
// function_ref default_arguments.ReabstractDefaultArgument.__allocating_init <A>(default_arguments.ReabstractDefaultArgument<A>.Type)(a : (A, A) -> Swift.Bool) -> default_arguments.ReabstractDefaultArgument<A>
// CHECK: [[FN:%.*]] = function_ref @$s17default_arguments25ReabstractDefaultArgument{{.*}} : $@convention(thin) <τ_0_0> () -> @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Bool for <τ_0_0, τ_0_0>
// CHECK-NEXT: [[RESULT:%.*]] = apply [[FN]]<Int>() : $@convention(thin) <τ_0_0> () -> @owned @callee_guaranteed @substituted <τ_0_0, τ_0_1> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_1) -> Bool for <τ_0_0, τ_0_0>
// CHECK-NEXT: [[RESULT_CONV:%.*]] = convert_function [[RESULT]]
// CHECK-NEXT: function_ref reabstraction thunk helper from @escaping @callee_guaranteed (@in_guaranteed Swift.Int, @in_guaranteed Swift.Int) -> (@unowned Swift.Bool) to @escaping @callee_guaranteed (@unowned Swift.Int, @unowned Swift.Int) -> (@unowned Swift.Bool)
// CHECK-NEXT: [[THUNK:%.*]] = function_ref @$sS2iSbIegnnd_S2iSbIegyyd_TR : $@convention(thin) (Int, Int, @guaranteed @callee_guaranteed (@in_guaranteed Int, @in_guaranteed Int) -> Bool) -> Bool
// CHECK-NEXT: [[FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[RESULT_CONV]]) : $@convention(thin) (Int, Int, @guaranteed @callee_guaranteed (@in_guaranteed Int, @in_guaranteed Int) -> Bool) -> Bool
// CHECK-NEXT: [[CONV_FN:%.*]] = convert_escape_to_noescape [not_guaranteed] [[FN]]
// function_ref reabstraction thunk helper from @callee_guaranteed (@unowned Swift.Int, @unowned Swift.Int) -> (@unowned Swift.Bool) to @callee_guaranteed (@in_guaranteed Swift.Int, @in_guaranteed Swift.Int) -> (@unowned Swift.Bool)
// CHECK: [[THUNK:%.*]] = function_ref @$sS2iSbIgyyd_S2iSbIegnnd_TR :
// CHECK-NEXT: [[FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[CONV_FN]]) :
// CHECK-NEXT: [[CONV_FN_0:%.*]] = convert_function [[FN]]
// CHECK-NEXT: [[CONV_FN:%.*]] = convert_escape_to_noescape [not_guaranteed] [[CONV_FN_0]]
// CHECK: [[INITFN:%[0-9]+]] = function_ref @$s17default_arguments25ReabstractDefaultArgumentC{{[_0-9a-zA-Z]*}}fC
// CHECK-NEXT: apply [[INITFN]]<Int>([[CONV_FN]], 

func testDefaultArgumentReabstraction() {
  _ = ReabstractDefaultArgument<Int>()
}

// <rdar://problem/20494437> SILGen crash handling default arguments
// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments18r20494437onSuccessyyAA25r20494437ExecutionContext_pF
// CHECK: function_ref @$s17default_arguments19r20494437onCompleteyyAA25r20494437ExecutionContext_pF
// <rdar://problem/20494437> SILGen crash handling default arguments
protocol r20494437ExecutionContext {}
let r20494437Default: r20494437ExecutionContext
func r20494437onComplete(_ executionContext: r20494437ExecutionContext = r20494437Default) {}
func r20494437onSuccess(_ a: r20494437ExecutionContext) {
  r20494437onComplete(a)
}

// <rdar://problem/18400194> Parenthesized function expression crashes the compiler
func r18400194(_ a: Int, x: Int = 97) {}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments9r18400194_1xySi_SitFfA0_
// CHECK: integer_literal $Builtin.IntLiteral, 97

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments14test_r18400194yyF
// CHECK: integer_literal $Builtin.IntLiteral, 1
// CHECK:  function_ref @$s17default_arguments9r18400194_1xySi_SitFfA0_ : $@convention(thin) () -> Int
// CHECK: function_ref @$s17default_arguments9r18400194_1xySi_SitF : $@convention(thin) (Int, Int) -> (){{.*}}
func test_r18400194() {
  (r18400194)(1)
}

// rdar://24242783
//   Don't add capture arguments to local default argument generators.
func localFunctionWithDefaultArg() {
  var z = 5
  func bar(_ x: Int? = (nil)) {
    z += 1
  }
  bar()
}
// CHECK-LABEL: sil private [ossa] @$s17default_arguments27localFunctionWithDefaultArgyyF3barL_yySiSgFfA_
// CHECK-SAME: $@convention(thin) () -> Optional<Int>

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments15throwingDefault7closureySbyKXE_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
func throwingDefault(closure: () throws -> Bool  = {  return true }) throws {
  try _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments26throwingAutoclosureDefault7closureySbyKXK_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
func throwingAutoclosureDefault(closure: @autoclosure () throws -> Bool  = true ) throws {
  try _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments0A3Arg7closureySbyXE_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
func defaultArg(closure: () -> Bool  = {  return true }) {
  _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments21autoclosureDefaultArg7closureySbyXK_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
func autoclosureDefaultArg(closure: @autoclosure () -> Bool  = true ) {
  _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments23throwingDefaultEscaping7closureySbyKc_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
func throwingDefaultEscaping(closure: @escaping () throws -> Bool  = {  return true }) throws {
  try _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments34throwingAutoclosureDefaultEscaping7closureySbyKXA_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
func throwingAutoclosureDefaultEscaping(closure: @escaping @autoclosure () throws -> Bool  = true ) throws {
  try _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments0A8Escaping7closureySbyc_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
func defaultEscaping(closure: @escaping () -> Bool  = {  return true }) {
  _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments26autoclosureDefaultEscaping7closureySbyXA_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool {
func autoclosureDefaultEscaping(closure: @escaping @autoclosure () -> Bool  = true ) {
  _ = closure()
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}callThem{{.*}} : $@convention(thin) () -> @error any Error

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments15throwingDefault7closureySbyKXE_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[C:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[E:%.*]] = convert_escape_to_noescape [not_guaranteed] [[C]]
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments15throwingDefault7closureySbyKXE_tKF :
// CHECK:  try_apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments26throwingAutoclosureDefault7closureySbyKXK_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[C:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[E:%.*]] = convert_escape_to_noescape [not_guaranteed] [[C]]
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments26throwingAutoclosureDefault7closureySbyKXK_tKF :
// CHECK:  try_apply [[R]]([[E]])

// CHECK:   [[F:%.*]] = function_ref @$s17default_arguments0A3Arg7closureySbyXE_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:   [[C:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:   [[E:%.*]] = convert_escape_to_noescape [not_guaranteed] [[C]]
// CHECK:   [[R:%.*]] = function_ref @$s17default_arguments0A3Arg7closureySbyXE_tF :
// CHECK:   apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments21autoclosureDefaultArg7closureySbyXK_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Boo
// CHECK:  [[C:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:  [[E:%.*]] = convert_escape_to_noescape [not_guaranteed] [[C]]
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments21autoclosureDefaultArg7closureySbyXK_tF :
// CHECK:  apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments23throwingDefaultEscaping7closureySbyKc_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[E:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments23throwingDefaultEscaping7closureySbyKc_tKF : $@convention(thin) (@guaranteed @callee_guaranteed () -> (Bool, @error any Error)) -> @error any Error
// CHECK:  try_apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments34throwingAutoclosureDefaultEscaping7closureySbyKXA_tKFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[E:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> (Bool, @error any Error)
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments34throwingAutoclosureDefaultEscaping7closureySbyKXA_tKF : $@convention(thin) (@guaranteed @callee_guaranteed () -> (Bool, @error any Error)) -> @error any Error
// CHECK:  try_apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments0A8Escaping7closureySbyc_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:  [[E:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments0A8Escaping7closureySbyc_tF : $@convention(thin) (@guaranteed @callee_guaranteed () -> Bool) -> ()
// CHECK:  apply [[R]]([[E]])

// CHECK:  [[F:%.*]] = function_ref @$s17default_arguments26autoclosureDefaultEscaping7closureySbyXA_tFfA_ : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:  [[E:%.*]] = apply [[F]]() : $@convention(thin) () -> @owned @callee_guaranteed () -> Bool
// CHECK:  [[R:%.*]] = function_ref @$s17default_arguments26autoclosureDefaultEscaping7closureySbyXA_tF : $@convention(thin) (@guaranteed @callee_guaranteed () -> Bool) -> ()
// CHECK:  apply [[R]]([[E]])

func callThem() throws {
   try throwingDefault()
   try throwingAutoclosureDefault()
   defaultArg()
   autoclosureDefaultArg()
   try throwingDefaultEscaping()
   try throwingAutoclosureDefaultEscaping()
   defaultEscaping()
   autoclosureDefaultEscaping()
}

func tupleDefaultArg(x: (Int, Int) = (1, 2)) {}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments19callTupleDefaultArgyyF : $@convention(thin) () -> ()
// CHECK: function_ref @$s17default_arguments15tupleDefaultArg1xySi_Sit_tFfA_ : $@convention(thin) () -> (Int, Int)
// CHECK: function_ref @$s17default_arguments15tupleDefaultArg1xySi_Sit_tF : $@convention(thin) (Int, Int) -> ()
// CHECK: return
func callTupleDefaultArg() {
  tupleDefaultArg()
}

// FIXME: Should this be banned?
func stupidGames(x: Int = 3) -> Int {
  return x
}
stupidGames(x:)()

func genericMagic<T : ExpressibleByStringLiteral>(x: T = #file) -> T {
  return x
}

let _: String = genericMagic()

// https://github.com/apple/swift/issues/54185

struct CallableWithDefault {
  func callAsFunction(x: Int = 4) {}
  func callAsFunction(y: Int, z: String = #function) {}
}

// CHECK-LABEL: sil hidden [ossa] @$s17default_arguments23testCallableWithDefaultyyAA0deF0VF : $@convention(thin) (CallableWithDefault) -> ()
func testCallableWithDefault(_ x: CallableWithDefault) {
  // CHECK: [[DEF_FN:%[0-9]+]] = function_ref @$s17default_arguments19CallableWithDefaultV14callAsFunction1xySi_tFfA_ : $@convention(thin) () -> Int
  // CHECK: [[DEF:%[0-9]+]] = apply [[DEF_FN]]() : $@convention(thin) () -> Int
  // CHECK: [[CALL_AS_FN:%[0-9]+]] = function_ref @$s17default_arguments19CallableWithDefaultV14callAsFunction1xySi_tF : $@convention(method) (Int, CallableWithDefault) -> ()
  // CHECK: apply [[CALL_AS_FN]]([[DEF]], {{%[0-9]+}})
  x()

  // CHECK: [[RAW_I:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 5
  // CHECK: [[I:%[0-9]+]] = apply {{%[0-9]+}}([[RAW_I]], {{%[0-9]+}}) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
  // CHECK: [[RAW_STR:%[0-9]+]] = string_literal utf8 "testCallableWithDefault(_:)"
  // CHECK: [[STR:%[0-9]+]] = apply {{%[0-9]+}}([[RAW_STR]], {{%[0-9]+}}, {{%[0-9]+}}, {{%[0-9]+}}) : $@convention(method) (Builtin.RawPointer, Builtin.Word, Builtin.Int1, @thin String.Type) -> @owned String
  // CHECK: [[CALL_AS_FN:%[0-9]+]] = function_ref @$s17default_arguments19CallableWithDefaultV14callAsFunction1y1zySi_SStF : $@convention(method) (Int, @guaranteed String, CallableWithDefault) -> ()
  // CHECK: apply [[CALL_AS_FN]]([[I]], [[STR]], {{%[0-9]+}})
  x(y: 5)
}

enum E {
  // CHECK-LABEL: sil hidden [ossa] @$s17default_arguments1EO6ResultV4name9platformsAESS_SaySiGtcfcfA0_ : $@convention(thin) () -> @owned Array<Int>
  struct Result {
    var name: String
    var platforms: [Int] = []
  }

  // CHECK-LABEL: sil hidden [ossa] @$s17default_arguments1EO4testyyFZ : $@convention(method) (@thin E.Type) -> ()
  static func test() {
    // CHECK: function_ref @$s17default_arguments1EO6ResultV4name9platformsAESS_SaySiGtcfcfA0_ : $@convention(thin) () -> @owned Array<Int>
    // CHECK: function_ref @$s17default_arguments1EO6ResultV4name9platformsAESS_SaySiGtcfC : $@convention(method) (@owned String, @owned Array<Int>, @thin E.Result.Type) -> @owned E.Result

    var result = Self.Result(name: "")
  }
  // CHECK: end sil function '$s17default_arguments1EO4testyyFZ'
}

// FIXME: Arguably we shouldn't allow calling a constructor like this, as
// we usually require the user write an explicit '.init'.
struct WeirdUMEInitCase {
  static let ty = WeirdUMEInitCase.self
  init(_ x: Int = 0) {}
}

let _: WeirdUMEInitCase = .ty()
let _: WeirdUMEInitCase = .ty(5)

struct KeyPathLiteralAsFunctionDefaultArg {
    var x: Int

    func doStuff(with prop: (KeyPathLiteralAsFunctionDefaultArg) -> Int = \.x) {}
}

KeyPathLiteralAsFunctionDefaultArg(x: 1738).doStuff()
