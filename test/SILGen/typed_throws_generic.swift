// RUN: %target-swift-emit-silgen %s -enable-experimental-feature FullTypedThrows | %FileCheck %s

// REQUIRES: asserts

public func genericThrow<E>(e: E) throws(E) {
  throw e
}


// CHECK-LABEL: sil [ossa] @$s20typed_throws_generic0C5Throw1eyx_txYKs5ErrorRzlF : $@convention(thin) <E where E : Error> (@in_guaranteed E) -> @error_indirect E {

// CHECK: bb0(%0 : $*E, %1 : $*E):
// CHECK: [[TMP:%.*]] = alloc_stack $E
// CHECK: copy_addr %1 to [init] [[TMP]] : $*E
// CHECK: copy_addr [take] [[TMP]] to [init] %0 : $*E
// CHECK: dealloc_stack [[TMP]] : $*E
// CHECK: throw_addr


public func genericTryApply<E>(fn: () throws(E) -> ()) throws(E) {
  try fn()
}

// CHECK-LABEL: sil [ossa] @$s20typed_throws_generic0C8TryApply2fnyyyxYKXE_txYKs5ErrorRzlF : $@convention(thin) <E where E : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>) -> @error_indirect E {

// CHECK: bb0(%0 : $*E, %1 : @guaranteed $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>):
// CHECK: [[FN:%.*]] = copy_value %1 : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: [[ERROR:%.*]] = alloc_stack $E
// CHECK: [[FN_BORROW:%.*]] = begin_borrow [[FN]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: try_apply [[FN_BORROW]]([[ERROR]]) : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>, normal bb1, error bb2

// CHECK: bb1(%7 : $()):
// CHECK: end_borrow [[FN_BORROW]]
// CHECK: dealloc_stack [[ERROR]]
// CHECK: destroy_value [[FN]]
// CHECK: return

// CHECK: bb2:
// CHECK: copy_addr [take] [[ERROR]] to [init] %0 : $*E
// CHECK: end_borrow [[FN_BORROW]]
// CHECK: dealloc_stack [[ERROR]]
// CHECK: destroy_value [[FN]]
// CHECK: throw_addr


public func genericOptionalTry<E>(fn: () throws(E) -> ()) -> ()? {
  return try? fn()
}

// CHECK-LABEL: sil [ossa] @$s20typed_throws_generic0C11OptionalTry2fnytSgyyxYKXE_ts5ErrorRzlF : $@convention(thin) <E where E : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>) -> Optional<()> {
// CHECK: bb0(%0 : @guaranteed $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>):
// CHECK: [[FN:%.*]] = copy_value %0 : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: [[ERROR:%.*]] = alloc_stack $E
// CHECK: [[FN_BORROW:%.*]] = begin_borrow %2 : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: try_apply [[FN_BORROW]]([[ERROR]]) : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>, normal bb1, error bb3

// CHECK: bb1({{.*}} : $()):
// CHECK: end_borrow [[FN_BORROW]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: dealloc_stack [[ERROR]] : $*E
// CHECK: [[RESULT:%.*]] = tuple ()
// CHECK: [[OPT:%.*]] = enum $Optional<()>, #Optional.some!enumelt, [[RESULT]] : $()
// CHECK: destroy_value [[FN]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: br bb2([[OPT]] : $Optional<()>)

// CHECK: bb2([[RESULT:%.*]] : $Optional<()>):
// CHECK: return [[RESULT]] : $Optional<()>

// CHECK: bb3:
// CHECK: destroy_addr [[ERROR]] : $*E
// CHECK: end_borrow [[FN_BORROW]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: dealloc_stack [[ERROR]] : $*E
// CHECK: destroy_value [[FN]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: [[RESULT:%.*]] = enum $Optional<()>, #Optional.none!enumelt
// CHECK: br bb2([[RESULT]] : $Optional<()>)


public func genericForceTry<E>(fn: () throws(E) -> ()) {
  try! fn()
}

// CHECK-LABEL: sil [ossa] @$s20typed_throws_generic0C8ForceTry2fnyyyxYKXE_ts5ErrorRzlF : $@convention(thin) <E where E : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>) -> () {
// CHECK: bb0(%0 : @guaranteed $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>):
// CHECK: [[OUTER_ERROR:%.*]] = alloc_stack $E
// CHECK: [[FN:%.*]] = copy_value %0 : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: [[ERROR:%.*]] = alloc_stack $E
// CHECK: [[FN_BORROW:%.*]] = begin_borrow [[FN]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: try_apply [[FN_BORROW]]([[ERROR]]) : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>, normal bb1, error bb2

// CHECK: bb1({{.*}} : $()):
// CHECK: end_borrow [[FN_BORROW]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: dealloc_stack [[ERROR]] : $*E
// CHECK: [[RESULT:%.*]] = tuple ()
// CHECK: return [[RESULT]] : $()

// CHECK: bb2:
// CHECK: copy_addr [take] [[ERROR]] to [init] [[OUTER_ERROR]] : $*E
// CHECK: end_borrow [[FN_BORROW]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
// CHECK: dealloc_stack [[ERROR]] : $*E
// CHECK: destroy_value [[FN]] : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> @error_indirect τ_0_0 for <E>
  // CHECK: [[UNEXPECTED_FN:%.*]] = function_ref @swift_unexpectedErrorTyped : $@convention(thin) <τ_0_0 where τ_0_0 : Error> (@in τ_0_0, Builtin.RawPointer, Builtin.Word, Builtin.Int1, Builtin.Word) -> ()
  // CHECK-NEXT: apply [[UNEXPECTED_FN]]<E>([[OUTER_ERROR]], {{[^)]*}}) : $@convention(thin) <τ_0_0 where τ_0_0 : Error> (@in τ_0_0, Builtin.RawPointer, Builtin.Word, Builtin.Int1, Builtin.Word) -> ()
// CHECK-NEXT: unreachable

enum MyError: Error {
case fail
}

func passthroughCall<T, E>(_ body: () throws(E) -> T) throws(E) -> T {
  try body()
}

func five() -> Int { 5 }

func fiveOrBust() throws -> Int { 5 }

func fiveOrTypedBust() throws(MyError) -> Int { 5 }

// CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic23reabstractAsNonthrowingSiyF
func reabstractAsNonthrowing() -> Int {
  // CHECK: [[FN:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PRIOR_FN:%.*]] : $@callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int> to $@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int>
  // CHECK: [[CALLEE:%.*]] = function_ref @$s20typed_throws_generic15passthroughCallyxxyq_YKXEq_YKs5ErrorR_r0_lF : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1)
  // CHECK: [[ERROR_SLOT:%.*]] = alloc_stack $Never
  // CHECK: try_apply [[CALLEE]]<Int, Never>(%0, [[ERROR_SLOT]], [[FN]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
  passthroughCall(five)

  // CHECK: [[NORMAL_BB]]
  // CHECK: dealloc_stack [[ERROR_SLOT]] : $*Never

  // CHECK: [[ERROR_BB]]:
  // CHECK-NEXT: unreachable
}

// CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic20reabstractAsThrowingSiyKF
func reabstractAsThrowing() throws -> Int {
  // CHECK: [[FN:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PRIOR_FN:%.*]] : $@callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <any Error, Int> to $@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <any Error, Int>
  // CHECK: [[CALLEE:%.*]] = function_ref @$s20typed_throws_generic15passthroughCallyxxyq_YKXEq_YKs5ErrorR_r0_lF : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1)
  // CHECK: [[ERROR_SLOT:%.*]] = alloc_stack $any Error
  // CHECK: try_apply [[CALLEE]]<Int, any Error>(%1, [[ERROR_SLOT]], [[FN]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
  try passthroughCall(fiveOrBust)

  // CHECK: [[NORMAL_BB]]
  // CHECK: dealloc_stack [[ERROR_SLOT]] : $*any Error

  // CHECK: [[ERROR_BB]]:
  // CHECK: throw [[ERR:%.*]] : $any Error
}

// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$sSis5Error_pIgdzo_SisAA_pIegrzr_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> (Int, @error any Error)) -> (@out Int, @error_indirect any Error)
// CHECK: bb0([[ARG:%.*]] : $*Int, [[OUTER_ERROR_SLOT:%.*]] : $*any Error, [[INNER_FN:%.*]] : @guaranteed $@noescape @callee_guaranteed () -> (Int, @error any Error)):
// CHECK: try_apply [[INNER_FN]]() : $@noescape @callee_guaranteed () -> (Int, @error any Error), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]

// CHECK: [[NORMAL_BB]]([[RESULT:%.*]] : $Int):
// CHECK-NEXT: store [[RESULT]] to [trivial] [[ARG]] : $*Int
// CHECK-NEXT: [[VOID_RESULT:%.*]] = tuple ()
// CHECK-NEXT:  return [[VOID_RESULT]] : $()

// CHECK: [[ERROR_BB]]([[INNER_ERROR:%.*]] : @owned $any Error):
// CHECK-NEXT:  store [[INNER_ERROR]] to [init] [[OUTER_ERROR_SLOT]] : $*any Error
// CHECK-NEXT:  throw_addr

// CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic28reabstractAsConcreteThrowingSiyKF : $@convention(thin) () -> (Int, @error any Error)
func reabstractAsConcreteThrowing() throws -> Int {
  // CHECK: [[FN:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PRIOR_FN:%.*]] : $@callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <MyError, Int> to $@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <MyError, Int>
  // CHECK: [[CALLEE:%.*]] = function_ref @$s20typed_throws_generic15passthroughCallyxxyq_YKXEq_YKs5ErrorR_r0_lF : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1)
  // CHECK: [[ERROR_SLOT:%.*]] = alloc_stack $MyError
  // CHECK: try_apply [[CALLEE]]<Int, MyError>(%1, [[ERROR_SLOT]], [[FN]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
  try passthroughCall(fiveOrTypedBust)

  // CHECK: [[NORMAL_BB]]
  // CHECK-NEXT: dealloc_stack [[ERROR_SLOT]] : $*MyError

  // CHECK: [[ERROR_BB]]:
  // CHECK: alloc_existential_box $any Error, $MyError
  // CHECK-NEXT: project_existential_box $MyError in
  // CHECK: throw [[ERR:%.*]] : $any Error
}

// CHECK-LABEL: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$sSi20typed_throws_generic7MyErrorOIgdzo_SiACIegrzr_TR : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> (Int, @error MyError)) -> (@out Int, @error_indirect MyError) {
// CHECK: bb0([[RESULT_ADDR:%.*]] : $*Int, [[OUTER_ERROR:%.*]] : $*MyError, [[FN:%.*]] : @guaranteed $@noescape @callee_guaranteed () -> (Int, @error MyError)):
// CHECK-NEXT: try_apply [[FN]]() : $@noescape @callee_guaranteed () -> (Int, @error MyError), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]

// CHECK: [[NORMAL_BB]]([[INNER_RESULT:%.*]] : $Int):
// CHECK-NEXT: store [[INNER_RESULT]] to [trivial] [[RESULT_ADDR]] : $*Int

// CHECK: [[ERROR_BB]]([[INNER_ERROR:%.*]] : $MyError):
// CHECK-NEXT: store [[INNER_ERROR]] to [trivial] [[OUTER_ERROR]] : $*MyError
// CHECK-NEXT: throw_addr


// CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic30reabstractClosureAsNonthrowingSiyF : $@convention(thin) () -> Int
func reabstractClosureAsNonthrowing() -> Int {
  // CHECK: [[INT_BOX:%.*]] = alloc_stack $Int
  // CHECK: [[CLOSURE:%.*]] = function_ref @$s20typed_throws_generic30reabstractClosureAsNonthrowingSiyFSiyXEfU_ : $@convention(thin) @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int>
  // CHECK-NEXT: [[THICK_CLOSURE:%.*]] = thin_to_thick_function [[CLOSURE]] : $@convention(thin) @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int> to $@noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int>
  // CHECK: [[CALLEE:%.*]] = function_ref @$s20typed_throws_generic15passthroughCallyxxyq_YKXEq_YKs5ErrorR_r0_lF : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1)
  // CHECK-NEXT: [[NEVER_BOX:%.*]] = alloc_stack $Never
  // CHECK-NEXT: try_apply [[CALLEE]]<Int, Never>([[INT_BOX]], [[NEVER_BOX]], [[THICK_CLOSURE]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
  passthroughCall { 5 }

  // CHECK: [[NORMAL_BB]]
  // CHECK-NEXT: dealloc_stack [[NEVER_BOX]] : $*Never
  // CHECK-NEXT: [[RESULT:%.*]] = load [trivial] [[INT_BOX]] : $*Int
  // CHECK-NEXT: dealloc_stack [[INT_BOX]] : $*Int
  // CHECK-NEXT: return [[RESULT]] : $Int

  // CHECK: [[ERROR_BB]]:
  // CHECK-NEXT: unreachable

  // CHECK-LABEL: sil private [ossa] @$s20typed_throws_generic30reabstractClosureAsNonthrowingSiyFSiyXEfU_ : $@convention(thin) @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <Never, Int>
}

func reabstractClosureAsThrowing(b: Bool) throws -> Int {
  try passthroughCall {
    if b {
      throw MyError.fail
    }

    return 5
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic32reabstractClosureAsTypedThrowing1bSiSb_tAA7MyErrorOYKF : $@convention(thin) (Bool) -> (Int, @error MyError)
func reabstractClosureAsTypedThrowing(b: Bool) throws(MyError) -> Int {
  // CHECK: try_apply [[CALLEE:%.*]]<Int, MyError>([[INT_BOX:%.*]], [[ERROR_BOX:%.*]], [[THICK_CLOSURE:%.*]]) : $@convention(thin) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> () -> (@out τ_0_1, @error_indirect τ_0_0) for <τ_0_1, τ_0_0>) -> (@out τ_0_0, @error_indirect τ_0_1), normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
  try passthroughCall { () throws(MyError) -> Int in
    if b {
      throw MyError.fail
    }

    return 5
  }

  // CHECK: [[NORMAL_BB]]
  // CHECK-NEXT: dealloc_stack [[ERROR_BOX]] : $*MyError
  // CHECK: [[RESULT:%.*]] = load [trivial] [[INT_BOX]] : $*Int
  // CHECK-NEXT: dealloc_stack [[INT_BOX]] : $*Int
  // CHECK-NEXT: return [[RESULT]] : $Int

  // CHECK: [[ERROR_BB]]:
  // CHECK-NEXT: [[ERROR:%.*]] = load [trivial] [[ERROR_BOX]] : $*MyError
  // CHECK-NEXT: dealloc_stack [[ERROR_BOX]] : $*MyError
  // CHECK: dealloc_stack [[INT_BOX]] : $*Int
  // CHECK-NEXT: throw [[ERROR]] : $MyError
}


extension Collection {
  func typedMap<T, E>(body: (Element) throws(E) -> T) throws(E) -> [T] {
    var result: [T] = []
    for element in self {
      result.append(try body(element))
    }
    return result
  }
}

// CHECK-LABEL: sil private [ossa] @$s20typed_throws_generic9forcedMapySayq_GSayxGr0_lFq_xXEfU_ : $@convention(thin) <T, U> (@in_guaranteed T) -> (@out U, @error_indirect Never)
func forcedMap<T, U>(_ source: [T]) -> [U] {
  // CHECK: bb0(%0 : $*U, %1 : $*Never, %2 : $*T)
  return source.typedMap { $0 as! U }
}

// Witness thunks
protocol P {
  associatedtype E: Error
  func f() throws(E)
}

struct Res<Success, Failure: Error>: P {
  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic3ResVyxq_GAA1PA2aEP1fyy1EQzYKFTW : $@convention(witness_method: P) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@in_guaranteed Res<τ_0_0, τ_0_1>) -> @error_indirect τ_0_1
  // CHECK: bb0(%0 : $*τ_0_1, %1 : $*Res<τ_0_0, τ_0_1>):
  // CHECK: [[SELF:%.*]] = load [trivial] %1 : $*Res<τ_0_0, τ_0_1>
  // CHECK: [[WITNESS:%.*]] = function_ref @$s20typed_throws_generic3ResV1fyyq_YKF : $@convention(method) <τ_0_0, τ_0_1 where τ_0_1 : Error> (Res<τ_0_0, τ_0_1>) -> @error_indirect τ_0_1
  // CHECK-NEXT: [[INNER_ERROR_BOX:%.*]] = alloc_stack $τ_0_1
  // CHECK-NEXT: try_apply [[WITNESS]]<τ_0_0, τ_0_1>([[INNER_ERROR_BOX]], [[SELF]]) : $@convention(method) <τ_0_0, τ_0_1 where τ_0_1 : Error> (Res<τ_0_0, τ_0_1>) -> @error_indirect τ_0_1, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]

  // CHECK: [[NORMAL_BB]]
  // CHECK: dealloc_stack [[INNER_ERROR_BOX]] : $*τ_0_1

  // CHECK: [[ERROR_BB]]:
  // CHECK: throw_addr
  func f() throws(Failure) { }
}

struct TypedRes<Success>: P {
  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic8TypedResVyxGAA1PA2aEP1fyy1EQzYKFTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed TypedRes<τ_0_0>) -> @error_indirect MyError
  // CHECK: bb0(%0 : $*MyError, %1 : $*TypedRes<τ_0_0>)
  // CHECK: [[SELF:%.*]] = load [trivial] %1 : $*TypedRes<τ_0_0>
  // CHECK: [[WITNESS:%.*]] = function_ref @$s20typed_throws_generic8TypedResV1fyyAA7MyErrorOYKF : $@convention(method) <τ_0_0> (TypedRes<τ_0_0>) -> @error MyError
  // CHECK: try_apply [[WITNESS]]<τ_0_0>([[SELF]]) : $@convention(method) <τ_0_0> (TypedRes<τ_0_0>) -> @error MyError, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]

  // CHECK: [[NORMAL_BB]]
  // CHECK: return

  // CHECK: [[ERROR_BB]]([[ERROR:%.*]] : $MyError):
  // CHECK-NEXT: store [[ERROR]] to [trivial] %0 : $*MyError
  // CHECK-NEXT: throw_addr
  func f() throws(MyError) { }
}

struct UntypedRes<Success>: P {
  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic10UntypedResVyxGAA1PA2aEP1fyy1EQzYKFTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed UntypedRes<τ_0_0>) -> @error_indirect any Error
  // CHECK: bb0(%0 : $*any Error, %1 : $*UntypedRes<τ_0_0>):
  // CHECK: [[SELF:%.*]] = load [trivial] %1 : $*UntypedRes<τ_0_0>
  // CHECK: [[WITNESS:%.*]] = function_ref @$s20typed_throws_generic10UntypedResV1fyyKF : $@convention(method) <τ_0_0> (UntypedRes<τ_0_0>) -> @error any Error
  // CHECK: try_apply [[WITNESS]]<τ_0_0>([[SELF]]) : $@convention(method) <τ_0_0> (UntypedRes<τ_0_0>) -> @error any Error, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]

  // CHECK: [[NORMAL_BB]]
  // CHECK: return

  // CHECK: [[ERROR_BB]]([[ERROR:%.*]] : @owned $any Error):
  // CHECK-NEXT: store [[ERROR]] to [init] %0 : $*any Error
  // CHECK-NEXT: throw_addr
  func f() throws { }
}

struct InfallibleRes<Success>: P {
  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic13InfallibleResVyxGAA1PA2aEP1fyy1EQzYKFTW : $@convention(witness_method: P) <τ_0_0> (@in_guaranteed InfallibleRes<τ_0_0>) -> @error_indirect Never
  // CHECK: bb0(%0 : $*Never, %1 : $*InfallibleRes<τ_0_0>):
  // CHECK: [[SELF:%.*]] = load [trivial] %1 : $*InfallibleRes<τ_0_0>
  // CHECK: [[WITNESS:%.*]] = function_ref @$s20typed_throws_generic13InfallibleResV1fyyF : $@convention(method) <τ_0_0> (InfallibleRes<τ_0_0>) -> ()
  // CHECK: = apply [[WITNESS]]<τ_0_0>([[SELF]]) : $@convention(method) <τ_0_0> (InfallibleRes<τ_0_0>)
  func f() { }
}

// Protocol with a default implementation of its function with a
// thrown error type.
protocol P2 {
  associatedtype Failure: Error
  func f() throws(Failure)
}

extension P2 {
  func f() throws(Failure) { }
}

public func withUnsafeRawBuffer<T, E>(
    _ body: (UnsafeMutableRawBufferPointer) throws(E) -> Void
) throws(E) -> T {
  fatalError("boom")
}

// CHECK-LABEL: sil private [ossa] @$s20typed_throws_generic16withUnsafeBufferyxySryxGq_YKXEq_YKs5ErrorR_r0_lFySwq_YKXEfU_ : $@convention(thin) <T, E where E : Error> (UnsafeMutableRawBufferPointer, @guaranteed @noescape @callee_guaranteed @substituted <τ_0_0, τ_0_1> (UnsafeMutableBufferPointer<τ_0_0>) -> @error_indirect τ_0_1 for <T, E>) -> @error_indirect E {
// CHECK: try_apply{{.*}}@error_indirect τ_0_1 for <T, E>
public func withUnsafeBuffer<T, E>(
    _ body: (UnsafeMutableBufferPointer<T>) throws(E) -> Void
) throws(E) -> T {
    try withUnsafeRawBuffer { (buf_ptr: UnsafeMutableRawBufferPointer) throws(E) in
        try body(buf_ptr.bindMemory(to: T.self))
    }
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic1SVAA2P2A2aDP1fyy7FailureQzYKFTW : $@convention(witness_method: P2) (@in_guaranteed S) -> @error_indirect Never
// CHECK: bb0(%0 : $*Never, %1 : $*S)
struct S: P2 {
  typealias Failure = Never
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic2GSVyxGAA2P2A2aEP1fyy7FailureQzYKFTW : $@convention(witness_method: P2) <τ_0_0> (@in_guaranteed GS<τ_0_0>) -> @error_indirect Never {
// CHECK: bb0(%0 : $*Never, %1 : $*GS<τ_0_0>):
struct GS<T>: P2 {
  typealias Failure = Never
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic3GSFVyxq_GAA2P2A2aEP1fyy7FailureQzYKFTW : $@convention(witness_method: P2) <τ_0_0, τ_0_1 where τ_0_1 : Error> (@in_guaranteed GSF<τ_0_0, τ_0_1>) -> @error_indirect τ_0_1 {
// CHECK: bb0(%0 : $*τ_0_1, %1 : $*GSF<τ_0_0, τ_0_1>):
struct GSF<T, Failure: Error>: P2 {
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic4GSF2Vyxq_GAA2P2A2aEP1fyy7FailureQzYKFTW : $@convention(witness_method: P2) <τ_0_0, τ_0_1 where τ_0_0 : Error> (@in_guaranteed GSF2<τ_0_0, τ_0_1>) -> @error_indirect τ_0_0 {
// CHECK: bb0(%0 : $*τ_0_0, %1 : $*GSF2<τ_0_0, τ_0_1>):
struct GSF2<F: Error, T>: P2 {
  typealias Failure = F
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20typed_throws_generic3GSAVyxGAA2P2A2aEP1fyy7FailureQzYKFTW : $@convention(witness_method: P2) <τ_0_0> (@in_guaranteed GSA<τ_0_0>) -> @error_indirect any Error {
// CHECK: bb0(%0 : $*any Error, %1 : $*GSA<τ_0_0>):
struct GSA<T>: P2 {
  typealias Failure = any Error
}

struct ReducedError<T: Error> {}

extension ReducedError where T == MyError {
  // CHECK-LABEL: sil hidden [ossa] @$s20typed_throws_generic12ReducedErrorVA2A02MyE0ORszrlE05throwfE0yyAEYKF : $@convention(method) (ReducedError<MyError>) -> @error MyError {
  func throwMyError() throws(T) {
    throw MyError.fail
  }
}
