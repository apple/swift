
// First check the SIL.
// RUN: %target-swift-frontend -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test -emit-sil -primary-file %s | %FileCheck %s

// Also do an end-to-end test to check all components, including IRGen.
// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT
// REQUIRES: executable_test

struct MyError : Error {
	let _code : Int
	init(_ xx: Int) {
		_code = xx
	}
}

// CHECK-LABEL: sil hidden [noinline] @$s4test7testit1yS2icSbF : $@convention(thin) (Bool) -> @owned @callee_guaranteed (Int) -> Int {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test16generic_get_funcyxxcx_SbtlFSi_Tg5 :
// CHECK: [[CL:%[0-9]+]] = apply [[F]](%{{[0-9]+}}, %0) :
// CHECK: [[CL2:%[0-9]+]] = convert_function [[CL]]
// CHECK: [[TH:%[0-9]+]] = function_ref @$sS2iIegnr_S2iIegyd_TR : $@convention(thin) (Int, @guaranteed @callee_guaranteed (@in_guaranteed Int) -> @out Int) -> Int
// CHECK: [[RET:%[0-9]+]] = partial_apply [callee_guaranteed] [[TH]]([[CL2]])
// CHECK: return [[RET]] : $@callee_guaranteed (Int) -> Int
@inline(never)
func testit1(_ b: Bool) -> (Int) -> Int {
  return generic_get_func(27, b)
}

@inline(never)
func testit2() -> (Int, Int, Bool) -> Int {
  return concrete_get_func()
}

// No reabstraction thunk is needed because we directly apply the returned closure.

// CHECK-LABEL: sil hidden [noinline] @$s4test7testit3ySiSbF : $@convention(thin) (Bool) -> Int {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test8generic3yxx_xSbtlFSi_Tg5 : $@convention(thin) (Int, Int, Bool) -> Int
// CHECK: [[RET:%[0-9]+]] = apply [[F]]({{.*}}) : $@convention(thin) (Int, Int, Bool) -> Int
// CHECK: return [[RET]] : $Int
@inline(never)
func testit3(_ b: Bool) -> Int {
  return generic3(270, 28, b)
}

// CHECK-LABEL: sil hidden [noinline] @$s4test16testit1_throwingyS2iKcSbF : $@convention(thin) (Bool) -> @owned @callee_guaranteed (Int) -> (Int, @error any Error) {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test25generic_get_func_throwingyxxKcSblFSi_Tg5 :
// CHECK: [[CL:%[0-9]+]] = apply [[F]](%0) :
// CHECK: [[CONV:%[0-9]+]] = convert_function [[CL]]
// CHECK: [[TH:%[0-9]+]] = function_ref @$sS2is5Error_pIegnrzo_S2isAA_pIegydzo_TR :
// CHECK: [[RET:%[0-9]+]] = partial_apply [callee_guaranteed] [[TH]]([[CONV]])
// CHECK: return [[RET]] : $@callee_guaranteed (Int) -> (Int, @error any Error)
@inline(never)
func testit1_throwing(_ b: Bool) -> (Int) throws -> Int {
  return generic_get_func_throwing(b)
}

@inline(never)
func testit2_throwing() -> (Int, Bool) throws -> Int {
  return concrete_get_func_throwing()
}

// No reabstraction thunk is needed because we directly apply the returned closure.

// CHECK-LABEL: sil hidden [noinline] @$s4test16testit3_throwingySiSbF : $@convention(thin) (Bool) -> Int {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test17generic3_throwingyxx_SbtKlFSi_Tg5 : $@convention(thin) (Int, Bool) -> (Int, @error any Error)
// CHECK: try_apply [[F]](%{{[0-9]+}}, %0) : $@convention(thin) (Int, Bool) -> (Int, @error any Error), normal bb{{[0-9]+}}, error bb{{[0-9]+}}
// CHECK: }
@inline(never)
func testit3_throwing(_ b: Bool) -> Int {
  do {
    return try generic3_throwing(271, b)
  } catch {
    return error._code
  }
}


// We need a reabstraction thunk to convert from direct args/result to indirect
// args/result, which is expected in the returned closure.

// CHECK-LABEL: sil shared [noinline] @$s4test16generic_get_funcyxxcx_SbtlFSi_Tg5 :
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test16generic_get_funcyxxcx_SbtlF0B0L_yxxlFSi_TG5 :
// CHECK: [[PA:%[0-9]+]] = partial_apply [callee_guaranteed] [[F]](%1, %{{[0-9]+}}) :
// CHECK: [[CONV:%[0-9]+]] = convert_function [[PA]]
// CHECK: return [[CONV]] :
@inline(never)
func generic_get_func<T>(_ t1: T, _ b: Bool) -> (T) -> T {

	@inline(never)
	func generic(_ t2: T) -> T {
		return b ? t1 : t2
	}

	return generic
}

@inline(never)
func generic2<T>(_ t1: T, t2: T, b: Bool) -> T {
	return b ? t1 : t2
}

// No reabstraction thunk is needed because the returned closure expects direct
// args/result anyway.

// CHECK-LABEL: sil hidden [noinline] @$s4test17concrete_get_funcS2i_SiSbtcyF : $@convention(thin) () -> @owned @callee_guaranteed (Int, Int, Bool) -> Int {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test8generic2_2t21bxx_xSbtlFSi_Tg5 : $@convention(thin) (Int, Int, Bool) -> Int
// CHECK: [[RET:%[0-9]+]] = thin_to_thick_function [[F]] : $@convention(thin) (Int, Int, Bool) -> Int to $@callee_guaranteed (Int, Int, Bool) -> Int
// CHECK: return [[RET]] : $@callee_guaranteed (Int, Int, Bool) -> Int
@inline(never)
func concrete_get_func() -> (Int, Int, Bool) -> Int {
	return generic2
}

@inline(never)
func generic3<T>(_ t1: T, _ t2: T, _ b: Bool) -> T {
	return b ? t1 : t2
}

// The same three test cases again, but with throwing functions.

// We need a reabstraction thunk to convert from direct args/result to indirect
// args/result, which is expected in the returned closure.

// CHECK-LABEL: sil shared [noinline] @$s4test25generic_get_func_throwingyxxKcSblFSi_Tg5 :
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test25generic_get_func_throwingyxxKcSblF0B0L_yxxKlFSi_TG5 :
// CHECK: [[PA:%[0-9]+]] = partial_apply [callee_guaranteed] [[F]](%0) : $@convention(thin) (@in_guaranteed Int, Bool) -> (@out Int, @error any Error)
// CHECK: [[CONV:%[0-9]+]] = convert_function [[PA]]
// CHECK: return [[CONV]] :
@inline(never)
func generic_get_func_throwing<T>(_ b: Bool) -> (T) throws -> T {

	@inline(never)
	func generic(_ t2: T) throws -> T {
		if b {
			throw MyError(123)
		}
		return t2
	}

	return generic
}

@inline(never)
func generic2_throwing<T>(_ t1: T, b: Bool) throws -> T {
	if b {
		throw MyError(124)
	}
	return t1
}

// No reabstraction thunk is needed because the returned closure expects direct
// args/result anyway.

// CHECK-LABEL: sil hidden [noinline] @$s4test26concrete_get_func_throwingS2i_SbtKcyF : $@convention(thin) () -> @owned @callee_guaranteed (Int, Bool) -> (Int, @error any Error) {
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test17generic2_throwing_1bxx_SbtKlFSi_Tg5 : $@convention(thin) (Int, Bool) -> (Int, @error any Error)
// CHECK: [[RET:%[0-9]+]] = thin_to_thick_function [[F]] : $@convention(thin) (Int, Bool) -> (Int, @error any Error) to $@callee_guaranteed (Int, Bool) -> (Int, @error any Error)
// CHECK: return [[RET]] : $@callee_guaranteed (Int, Bool) -> (Int, @error any Error)
@inline(never)
func concrete_get_func_throwing() -> (Int, Bool) throws -> Int {
	return generic2_throwing
}

@inline(never)
func generic3_throwing<T>(_ t1: T, _ b: Bool) throws -> T {
	if b {
		throw MyError(125)
	}
	return t1
}

// CHECK-LABEL: sil shared [transparent] [thunk] @$s4test16generic_get_funcyxxcx_SbtlF0B0L_yxxlFSi_TG5 : $@convention(thin) (@in_guaranteed Int, Bool, @in_guaranteed Int) -> @out Int {
// CHECK: bb0(%0 : $*Int, %1 : $*Int, %2 : @closureCapture $Bool, %3 : @closureCapture $*Int):
// CHECK: [[LD1:%[0-9]+]] = load %1 : $*Int
// CHECK: [[LD2:%[0-9]+]] = load %3 : $*Int
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test16generic_get_funcyxxcx_SbtlF0B0L_yxxlFSi_Tg5 : $@convention(thin) (Int, Bool, Int) -> Int
// CHECK: [[RET:%[0-9]+]] = apply [[F]]([[LD1]], %2, [[LD2]])
// CHECK: store [[RET]] to %0 : $*Int
// CHECK: return %{{[0-9]*}} : $()

// CHECK-LABEL: sil shared [transparent] [thunk] @$s4test25generic_get_func_throwingyxxKcSblF0B0L_yxxKlFSi_TG5 : $@convention(thin) (@in_guaranteed Int, Bool) -> (@out Int, @error any Error) {
// CHECK: bb0(%0 : $*Int, %1 : $*Int, %2 : @closureCapture $Bool):
// CHECK: [[LD:%[0-9]+]] = load %1 : $*Int
// CHECK: [[F:%[0-9]+]] = function_ref @$s4test25generic_get_func_throwingyxxKcSblF0B0L_yxxKlFSi_Tg5 : $@convention(thin) (Int, Bool) -> (Int, @error any Error)
// CHECK: try_apply [[F]]([[LD]], %2) : $@convention(thin) (Int, Bool) -> (Int, @error any Error), normal bb1, error bb2
// CHECK: bb1([[NORMAL:%[0-9]+]] : $Int):
// CHECK: store [[NORMAL]] to %0 : $*Int
// CHECK: return %{{[0-9]*}} : $()
// CHECK: bb2([[ERROR:%[0-9]+]] : $any Error):
// CHECK: throw [[ERROR]] : $any Error


// The main program.
// Check if the generated executable produces the correct output.

// CHECK-OUTPUT: 18
print(testit1(false)(18))
// CHECK-OUTPUT: 27
print(testit1(true)(18))

// CHECK-OUTPUT: 4
print(testit2()(3, 4, false))
// CHECK-OUTPUT: 3
print(testit2()(3, 4, true))

// CHECK-OUTPUT: 28
print(testit3(false))
// CHECK-OUTPUT: 270
print(testit3(true))

var x: Int
do {
	x = try testit1_throwing(false)(19)
} catch {
	x = error._code
}
// CHECK-OUTPUT: 19
print(x)
do {
	x = try testit1_throwing(true)(19)
} catch {
	x = error._code
}
// CHECK-OUTPUT: 123
print(x)

do {
	x = try testit2_throwing()(20, false)
} catch {
	x = error._code
}
// CHECK-OUTPUT: 20
print(x)
do {
	x = try testit2_throwing()(20, true)
} catch {
	x = error._code
}
// CHECK-OUTPUT: 124
print(x)

// CHECK-OUTPUT: 271
print(testit3_throwing(false))
// CHECK-OUTPUT: 125
print(testit3_throwing(true))

