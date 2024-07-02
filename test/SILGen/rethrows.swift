
// RUN: %target-swift-emit-sil -module-name rethrows -Xllvm -sil-disable-pass=simplification -verify %s | %FileCheck %s

@discardableResult
func rethrower(_ fn: () throws -> Int) rethrows -> Int {
  return try fn()
}
func thrower() throws -> Int { return 0 }
func nonthrower() -> Int { return 0 }

// CHECK-LABEL: sil hidden @$s8rethrows5test0yyKF : $@convention(thin) () -> @error any Error {
// CHECK:       [[THROWER:%.*]] = function_ref @$s8rethrows7throwerSiyKF : $@convention(thin) () -> (Int, @error any Error)
// CHECK:       [[T0:%.*]] = thin_to_thick_function [[THROWER]]
// CHECK:       [[CVT:%.*]] = convert_escape_to_noescape [[T0]]
// CHECK:       [[RETHROWER:%.*]] = function_ref @$s8rethrows9rethroweryS2iyKXEKF :
// CHECK:       try_apply [[RETHROWER]]([[CVT]]) : {{.*}}, normal [[NORMAL:bb1]], error [[ERROR:bb2]]
// CHECK:     [[NORMAL]]([[RESULT_T0:%.*]] : $Int):
// CHECK-NEXT:  strong_release [[T0]]
// CHECK-NEXT:  [[T1:%.*]] = tuple ()
// CHECK-NEXT:  return [[T1]]
// CHECK:     [[ERROR]]([[RESULT_T0:%.*]] : $any Error):
// CHECK-NEXT:  strong_release [[T0]]
// CHECK-NEXT:  throw [[RESULT_T0]]
func test0() throws {
  try rethrower(thrower)
}

// CHECK-LABEL: sil hidden @$s8rethrows5test1yyKF : $@convention(thin) () -> @error any Error {
// CHECK:       [[CLOSURE:%.*]] = function_ref @$s8rethrows5test1yyKFSiyKXEfU_ : $@convention(thin) () -> (Int, @error any Error)
// CHECK:       [[T0:%.*]] = thin_to_thick_function [[CLOSURE]]
// CHECK:       [[RETHROWER:%.*]] = function_ref @$s8rethrows9rethroweryS2iyKXEKF :
// CHECK:       try_apply [[RETHROWER]]([[T0]]) :{{.*}}, normal [[NORMAL:bb1]], error [[ERROR:bb2]]
// CHECK:     [[NORMAL]]({{%.*}} : $Int):
// CHECK-NEXT:  [[T1:%.*]] = tuple ()
// CHECK-NEXT:  return [[T1]]
// CHECK:     [[ERROR]]([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:  throw [[ERROR]]
//   Closure.
// CHECK-LABEL: sil private @$s8rethrows5test1yyKFSiyKXEfU_ : $@convention(thin) () -> (Int, @error any Error) {
// CHECK:       [[THROWER:%.*]] = function_ref @$s8rethrows7throwerSiyKF : $@convention(thin) () -> (Int, @error any Error)
// CHECK:       [[T0:%.*]] = thin_to_thick_function [[THROWER]]
// CHECK:       [[CVT:%.*]] = convert_escape_to_noescape [[T0]]
// CHECK:       [[RETHROWER:%.*]] = function_ref @$s8rethrows9rethroweryS2iyKXEKF :
// CHECK:       try_apply [[RETHROWER]]([[CVT]]) : {{.*}}, normal [[NORMAL:bb1]], error [[ERROR:bb2]]
// CHECK:     [[NORMAL]]([[RESULT:%.*]] : $Int):
// CHECK-NEXT:  strong_release [[T0]]
// CHECK-NEXT:  return [[RESULT]]
// CHECK:     [[ERROR]]([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:  strong_release [[T0]]
// CHECK-NEXT:  throw [[ERROR]]
func test1() throws {
  try rethrower { try rethrower(thrower) }
}

// CHECK-LABEL: sil hidden @$s8rethrows5test2yyF : $@convention(thin) () -> () {
// CHECK:       [[NONTHROWER:%.*]] = function_ref @$s8rethrows10nonthrowerSiyF : $@convention(thin) () -> Int
// CHECK:       [[T0:%.*]] = thin_to_thick_function [[NONTHROWER]]
// CHECK:       [[T1:%.*]] = convert_function [[T0]] : $@callee_guaranteed () -> Int to $@callee_guaranteed () -> (Int, @error any Error)
// CHECK:       [[T2:%.*]] = convert_escape_to_noescape [[T1]]
// CHECK:       [[RETHROWER:%.*]] = function_ref @$s8rethrows9rethroweryS2iyKXEKF :
// CHECK:       try_apply [[RETHROWER]]([[T2]]) : {{.*}}, normal [[NORMAL:bb1]], error [[ERROR:bb2]]
// CHECK:     [[NORMAL]]([[T0:%.*]] : $Int):
// CHECK-NEXT:  strong_release [[T1]]
// CHECK-NEXT:  [[RESULT:%.*]] = tuple ()
// CHECK-NEXT:  return [[RESULT]]
// CHECK:     [[ERROR]]([[T0:%.*]] : $any Error):
// CHECK-NEXT:  strong_release [[T1]]
// CHECK-NEXT:  strong_release [[T0]]
// CHECK-NEXT:  unreachable
func test2() {
  rethrower(nonthrower)
}

// CHECK-LABEL: sil hidden @$s8rethrows5test3yyF : $@convention(thin) () -> () {
// CHECK:       [[CLOSURE:%.*]] = function_ref @$s8rethrows5test3yyFSiyXEfU_ :
// CHECK:       [[T0:%.*]] = thin_to_thick_function [[CLOSURE]]
// CHECK:       [[RETHROWER:%.*]] = function_ref @$s8rethrows9rethroweryS2iyKXEKF :
// CHECK:       try_apply [[RETHROWER]]([[T0]])
// CHECK:     [[NORMAL]]({{%.*}} : $Int):
// CHECK-NEXT:  [[RESULT:%.*]] = tuple ()
// CHECK-NEXT:  return [[RESULT]]
// CHECK:     [[ERROR]]([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:  strong_release [[ERROR]]
// CHECK-NEXT:  unreachable
// CHECK-LABEL: // end sil function '$s8rethrows5test3yyF'
func test3() {
  rethrower { rethrower(nonthrower) }
}
