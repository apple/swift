// RUN: %target-swift-emit-silgen -parse-stdlib -Xllvm -sil-print-debuginfo -verify -swift-version 5 -primary-file %s %S/Inputs/errors_other.swift | %FileCheck %s

import Swift

class Cat {}

enum HomeworkError : Error {
  case TooHard
  case TooMuch
  case CatAteIt(Cat)
  case CatHidIt(Cat)
}

func someValidPointer<T>() -> UnsafePointer<T> { fatalError() }
func someValidPointer<T>() -> UnsafeMutablePointer<T> { fatalError() }

// CHECK: sil hidden [ossa] @$s6errors10make_a_cat{{.*}}F : $@convention(thin) () -> (@owned Cat, @error Error) {
// CHECK:      [[T1:%.*]] = metatype $@thick Cat.Type 
// CHECK:      [[T0:%.*]] = function_ref @$s6errors3Cat{{.*}} : $@convention(method) (@thick Cat.Type) -> @owned Cat
// CHECK-NEXT: [[T2:%.*]] = apply [[T0]]([[T1]])
// CHECK-NEXT: return [[T2]] : $Cat
func make_a_cat() throws -> Cat {
  return Cat()
}

// CHECK: sil hidden [ossa] @$s6errors15dont_make_a_cat{{.*}}F : $@convention(thin) () -> (@owned Cat, @error Error) {
// CHECK:      [[T0:%.*]] = metatype $@thin HomeworkError.Type
// CHECK-NEXT: [[T1:%.*]] = enum $HomeworkError, #HomeworkError.TooHard!enumelt
// CHECK-NEXT: [[BOX:%.*]] = alloc_existential_box $Error, $HomeworkError
// CHECK-NEXT: [[ADDR:%.*]] = project_existential_box $HomeworkError in [[BOX]] : $Error
// CHECK-NEXT: store [[BOX]] to [init] [[BOXBUF:%.*]] :
// CHECK-NEXT: store [[T1]] to [init] [[ADDR]]
// CHECK-NEXT: [[BOX2:%.*]] = load [take] [[BOXBUF]]
// CHECK-NEXT: builtin "willThrow"
// CHECK-NEXT: dealloc_stack [[BOXBUF]]
// CHECK-NEXT: throw [[BOX2]]
func dont_make_a_cat() throws -> Cat {
  throw HomeworkError.TooHard
}

// CHECK: sil hidden [ossa] @$s6errors11dont_return{{.*}}F : $@convention(thin) <T> (@in_guaranteed T) -> (@out T, @error Error) {
// CHECK:      [[T0:%.*]] = metatype $@thin HomeworkError.Type
// CHECK-NEXT: [[T1:%.*]] = enum $HomeworkError, #HomeworkError.TooMuch!enumelt
// CHECK-NEXT: [[BOX:%.*]] = alloc_existential_box $Error, $HomeworkError
// CHECK-NEXT: [[ADDR:%.*]] = project_existential_box $HomeworkError in [[BOX]] : $Error
// CHECK-NEXT: store [[BOX]] to [init] [[BOXBUF:%.*]] :
// CHECK-NEXT: store [[T1]] to [init] [[ADDR]]
// CHECK-NEXT: [[BOX2:%.*]] = load [take] [[BOXBUF]]
// CHECK-NEXT: builtin "willThrow"
// CHECK-NEXT: dealloc_stack [[BOXBUF]]
// CHECK-NEXT: throw [[BOX2]]
func dont_return<T>(_ argument: T) throws -> T {
  throw HomeworkError.TooMuch
}

// CHECK:    sil hidden [ossa] @$s6errors16all_together_nowyAA3CatCSbF : $@convention(thin) (Bool) -> @owned Cat {
// CHECK:    bb0(%0 : $Bool):
// CHECK:      [[RET_TEMP:%.*]] = alloc_stack $Cat

//   Branch on the flag.
// CHECK:      cond_br {{%.*}}, [[FLAG_TRUE:bb[0-9]+]], [[FLAG_FALSE:bb[0-9]+]]

//   In the true case, call make_a_cat().
// CHECK:    [[FLAG_TRUE]]:
// CHECK:      [[MAC_FN:%.*]] = function_ref @$s6errors10make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK-NEXT: try_apply [[MAC_FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[MAC_NORMAL:bb[0-9]+]], error [[MAC_ERROR:bb[0-9]+]]
// CHECK:    [[MAC_NORMAL]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: br [[TERNARY_CONT:bb[0-9]+]]([[T0]] : $Cat)

//   In the false case, call dont_make_a_cat().
// CHECK:    [[FLAG_FALSE]]:
// CHECK:      [[DMAC_FN:%.*]] = function_ref @$s6errors15dont_make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK-NEXT: try_apply [[DMAC_FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[DMAC_NORMAL:bb[0-9]+]], error [[DMAC_ERROR:bb[0-9]+]]
// CHECK:    [[DMAC_NORMAL]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: br [[TERNARY_CONT]]([[T0]] : $Cat)

//   Merge point for the ternary operator.  Call dont_return with the result.
// CHECK:    [[TERNARY_CONT]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: [[ARG_TEMP:%.*]] = alloc_stack $Cat
// CHECK-NEXT: store [[T0]] to [init] [[ARG_TEMP]]
// CHECK:      [[DR_FN:%.*]] = function_ref @$s6errors11dont_return{{.*}} :
// CHECK-NEXT: try_apply [[DR_FN]]<Cat>([[RET_TEMP]], [[ARG_TEMP]]) : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @error Error), normal [[DR_NORMAL:bb[0-9]+]], error [[DR_ERROR:bb[0-9]+]]
// CHECK:    [[DR_NORMAL]]({{%.*}} : $()):
// CHECK-NEXT: destroy_addr [[ARG_TEMP]]
// CHECK-NEXT: dealloc_stack [[ARG_TEMP]]
// CHECK-NEXT: [[T0:%.*]] = load [take] [[RET_TEMP]] : $*Cat
// CHECK-NEXT: dealloc_stack [[RET_TEMP]]
// CHECK-NEXT: br [[RETURN:bb[0-9]+]]([[T0]] : $Cat)

//   Return block.
// CHECK:    [[RETURN]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: return [[T0]] : $Cat

//   Catch dispatch block.
// CHECK:    [[CATCH:bb[0-9]+]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK-NEXT: [[SRC_TEMP:%.*]] = alloc_stack $Error
// CHECK-NEXT: [[COPIED_BORROWED_ERROR:%.*]] = copy_value [[BORROWED_ERROR]]
// CHECK-NEXT: store [[COPIED_BORROWED_ERROR]] to [init] [[SRC_TEMP]]
// CHECK-NEXT: [[DEST_TEMP:%.*]] = alloc_stack $HomeworkError
// CHECK-NEXT: checked_cast_addr_br copy_on_success Error in [[SRC_TEMP]] : $*Error to HomeworkError in [[DEST_TEMP]] : $*HomeworkError, [[IS_HWE:bb[0-9]+]], [[NOT_HWE:bb[0-9]+]]

//   Catch HomeworkError.
// CHECK:    [[IS_HWE]]:
// CHECK-NEXT: [[T0_ORIG:%.*]] = load [take] [[DEST_TEMP]] : $*HomeworkError
// CHECK-NEXT: switch_enum [[T0_ORIG]] : $HomeworkError, case #HomeworkError.CatAteIt!enumelt: [[MATCH:bb[0-9]+]], default [[NO_MATCH:bb[0-9]+]]

//   Catch HomeworkError.CatAteIt.
// CHECK:    [[MATCH]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: [[BORROWED_T0:%.*]] = begin_borrow [[T0]]
// CHECK-NEXT: [[T0_COPY:%.*]] = copy_value [[BORROWED_T0]]
// CHECK-NEXT: end_borrow [[BORROWED_T0]]
// CHECK-NEXT: destroy_value [[T0]]
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: br [[RETURN]]([[T0_COPY]] : $Cat)

//   Catch other HomeworkErrors.
// CHECK:    [[NO_MATCH]]([[CATCHALL_ERROR:%.*]] : @owned $HomeworkError):
// CHECK-NEXT: destroy_value [[CATCHALL_ERROR]]
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: br [[CATCHALL:bb[0-9]+]]

//   Catch other types.
// CHECK:    [[NOT_HWE]]:
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: br [[CATCHALL:bb[0-9]+]]

//   Catch all.
// CHECK:    [[CATCHALL]]:
// CHECK-NEXT: [[T1:%.*]] = metatype $@thick Cat.Type
// CHECK:      [[T0:%.*]] = function_ref @$s6errors3Cat{{.*}} : $@convention(method) (@thick Cat.Type) -> @owned Cat
// CHECK-NEXT: [[T2:%.*]] = apply [[T0]]([[T1]])
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: destroy_value [[ERROR]] : $Error
// CHECK-NEXT: br [[RETURN]]([[T2]] : $Cat)

//   Landing pad.
// CHECK:    [[MAC_ERROR]]([[T0:%.*]] : @owned $Error):
// CHECK-NEXT: dealloc_stack [[RET_TEMP]]
// CHECK-NEXT: br [[CATCH]]([[T0]] : $Error)
// CHECK:    [[DMAC_ERROR]]([[T0:%.*]] : @owned $Error):
// CHECK-NEXT: dealloc_stack [[RET_TEMP]]
// CHECK-NEXT: br [[CATCH]]([[T0]] : $Error)
// CHECK:    [[DR_ERROR]]([[T0:%.*]] : @owned $Error):
// CHECK-NEXT: destroy_addr [[CAT:%.*]] :
// CHECK-NEXT: dealloc_stack [[CAT]]
// CHECK-NEXT: dealloc_stack
// CHECK-NEXT: br [[CATCH]]([[T0]] : $Error)
func all_together_now(_ flag: Bool) -> Cat {
  do {
    return try dont_return(flag ? make_a_cat() : dont_make_a_cat())
  } catch HomeworkError.CatAteIt(let cat) {
    return cat
  } catch _ {
    return Cat()
  }
}

// Make sure that if we catch an error in a throwing function we borrow the
// error and only consume the error in the rethrow block.
//
// CHECK-LABEL: sil hidden [ossa] @$s6errors20all_together_now_twoyAA3CatCSgSbKF : $@convention(thin) (Bool) -> (@owned Optional<Cat>, @error Error) {
// CHECK: bb0(
// CHECK-NOT: bb1
// CHECK:   try_apply {{.*}}, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
//
// CHECK: [[ERROR_BB]]([[ERROR:%.*]] : @owned $Error):
// CHECK:   [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK:   [[COPIED_ERROR:%.*]] = copy_value [[BORROWED_ERROR]]
// CHECK:   store [[COPIED_ERROR]] to [init] [[CAST_INPUT_MEM:%.*]] : $*Error
// CHECK:   checked_cast_addr_br copy_on_success Error in [[CAST_INPUT_MEM]] : $*Error to HomeworkError in [[CAST_OUTPUT_MEM:%.*]] : $*HomeworkError, [[CAST_YES_BB:bb[0-9]+]], [[CAST_NO_BB:bb[0-9]+]],
//
// CHECK: [[CAST_YES_BB]]:
// CHECK:   [[SUBERROR:%.*]] = load [take] [[CAST_OUTPUT_MEM]]
// CHECK:   switch_enum [[SUBERROR]] : $HomeworkError, case #HomeworkError.TooHard!enumelt: {{bb[0-9]+}}, default [[SWITCH_MATCH_FAIL_BB:bb[0-9]+]],
//
// CHECK: [[SWITCH_MATCH_FAIL_BB]]([[SUBERROR:%.*]] : @owned $HomeworkError):
// CHECK:   destroy_value [[SUBERROR]]
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB:bb[0-9]+]]([[ERROR]] : $Error)
//
// CHECK: [[CAST_NO_BB]]:
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB]]([[ERROR]] : $Error)
//
// CHECK: [[RETHROW_BB]]([[ERROR_FOR_RETHROW:%.*]] : @owned $Error):
// CHECK:   throw [[ERROR_FOR_RETHROW]]
// CHECK: } // end sil function '$s6errors20all_together_now_twoyAA3CatCSgSbKF'
func all_together_now_two(_ flag: Bool) throws -> Cat? {
  do {
    return try dont_return(Cat())
  } catch HomeworkError.TooHard {
    return nil
  }
}

// Same as the previous test, but with multiple cases instead of just one.
//
// CHECK-LABEL: sil hidden [ossa] @$s6errors22all_together_now_threeyAA3CatCSgSbKF : $@convention(thin) (Bool) -> (@owned Optional<Cat>, @error Error) {
// CHECK: bb0(
// CHECK-NOT: bb1
// CHECK:   try_apply {{.*}}, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
//
// CHECK: [[ERROR_BB]]([[ERROR:%.*]] : @owned $Error):
// CHECK:   [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK:   [[COPIED_ERROR:%.*]] = copy_value [[BORROWED_ERROR]]
// CHECK:   store [[COPIED_ERROR]] to [init] [[CAST_INPUT_MEM:%.*]] : $*Error
// CHECK:   checked_cast_addr_br copy_on_success Error in [[CAST_INPUT_MEM]] : $*Error to HomeworkError in [[CAST_OUTPUT_MEM:%.*]] : $*HomeworkError, [[CAST_YES_BB:bb[0-9]+]], [[CAST_NO_BB:bb[0-9]+]],
//
// CHECK: [[CAST_YES_BB]]:
// CHECK:   [[SUBERROR:%.*]] = load [take] [[CAST_OUTPUT_MEM]]
// CHECK:   switch_enum [[SUBERROR]] : $HomeworkError, case #HomeworkError.TooHard!enumelt: {{bb[0-9]+}}, case #HomeworkError.TooMuch!enumelt: {{bb[0-9]+}}, default [[SWITCH_MATCH_FAIL_BB:bb[0-9]+]],
//
// CHECK: [[SWITCH_MATCH_FAIL_BB]]([[SUBERROR:%.*]] : @owned $HomeworkError):
// CHECK:   destroy_value [[SUBERROR]]
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB:bb[0-9]+]]([[ERROR]] : $Error)
//
// CHECK: [[CAST_NO_BB]]:
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB]]([[ERROR]] : $Error)
//
// CHECK: [[RETHROW_BB]]([[ERROR_FOR_RETHROW:%.*]] : @owned $Error):
// CHECK:   throw [[ERROR_FOR_RETHROW]]
// CHECK: } // end sil function '$s6errors22all_together_now_threeyAA3CatCSgSbKF'
func all_together_now_three(_ flag: Bool) throws -> Cat? {
  do {
    return try dont_return(Cat())
  } catch HomeworkError.TooHard {
    return nil
  } catch HomeworkError.TooMuch {
    return nil
  }
}

// Same as the previous test, but with a multi-pattern catch instead of two separate ones.
//
// CHECK-LABEL: sil hidden [ossa] @$s6errors21all_together_now_fouryAA3CatCSgSbKF : $@convention(thin) (Bool) -> (@owned Optional<Cat>, @error Error) {
// CHECK: bb0(
// CHECK-NOT: bb1
// CHECK:   try_apply {{.*}}, normal [[NORMAL_BB:bb[0-9]+]], error [[ERROR_BB:bb[0-9]+]]
//
// CHECK: [[ERROR_BB]]([[ERROR:%.*]] : @owned $Error):
// CHECK:   [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK:   [[COPIED_ERROR:%.*]] = copy_value [[BORROWED_ERROR]]
// CHECK:   store [[COPIED_ERROR]] to [init] [[CAST_INPUT_MEM:%.*]] : $*Error
// CHECK:   checked_cast_addr_br copy_on_success Error in [[CAST_INPUT_MEM]] : $*Error to HomeworkError in [[CAST_OUTPUT_MEM:%.*]] : $*HomeworkError, [[CAST_YES_BB:bb[0-9]+]], [[CAST_NO_BB:bb[0-9]+]],
//
// CHECK: [[CAST_YES_BB]]:
// CHECK:   [[SUBERROR:%.*]] = load [take] [[CAST_OUTPUT_MEM]]
// CHECK:   switch_enum [[SUBERROR]] : $HomeworkError, case #HomeworkError.TooHard!enumelt: [[TOO_HARD_BB:bb[0-9]+]], case #HomeworkError.TooMuch!enumelt: [[TOO_MUCH_BB:bb[0-9]+]], default [[SWITCH_MATCH_FAIL_BB:bb[0-9]+]],
//
// CHECK: [[TOO_HARD_BB]]:
// CHECK:   br [[CASE_BODY_BB:bb[0-9]+]]
//
// CHECK: [[TOO_MUCH_BB]]:
// CHECK:   br [[CASE_BODY_BB]]
//
// CHECK: [[CASE_BODY_BB]]
// CHECK:   [[RETVAL:%.*]] = enum $Optional<Cat>
// CHECK:   destroy_value [[ERROR]]
// CHECK:   br bb2([[RETVAL]] : $Optional<Cat>)
//
// CHECK: [[SWITCH_MATCH_FAIL_BB]]([[SUBERROR:%.*]] : @owned $HomeworkError):
// CHECK:   destroy_value [[SUBERROR]]
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB:bb[0-9]+]]([[ERROR]] : $Error)
//
// CHECK: [[CAST_NO_BB]]:
// CHECK:   end_borrow [[BORROWED_ERROR]]
// CHECK:   br [[RETHROW_BB]]([[ERROR]] : $Error)
//
// CHECK: [[RETHROW_BB]]([[ERROR_FOR_RETHROW:%.*]] : @owned $Error):
// CHECK:   throw [[ERROR_FOR_RETHROW]]
// CHECK: } // end sil function '$s6errors21all_together_now_fouryAA3CatCSgSbKF'
func all_together_now_four(_ flag: Bool) throws -> Cat? {
  do {
    return try dont_return(Cat())
  } catch HomeworkError.TooHard, HomeworkError.TooMuch {
    return nil
  }
}

// A multi-pattern catch with associated value bindings.
//
// CHECK-LABEL: sil hidden [ossa] @$s6errors21all_together_now_fiveyAA3CatCSbKF : $@convention(thin) (Bool) -> (@owned Cat, @error Error) {

// Return block.
// CHECK:    [[RETURN:bb[0-9]+]]([[RETVAL:%.*]] : @owned $Cat):
// CHECK-NEXT: return [[RETVAL]] : $Cat

//   Catch dispatch block.
// CHECK:    [[CATCH:bb[0-9]+]]([[ERROR:%.*]] : @owned $Error):
// CHECK:    [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK-NEXT: [[SRC_TEMP:%.*]] = alloc_stack $Error
// CHECK-NEXT: [[COPIED_BORROWED_ERROR:%.*]] = copy_value [[BORROWED_ERROR]]
// CHECK-NEXT: store [[COPIED_BORROWED_ERROR]] to [init] [[SRC_TEMP]]
// CHECK-NEXT: [[DEST_TEMP:%.*]] = alloc_stack $HomeworkError
// CHECK-NEXT: checked_cast_addr_br copy_on_success Error in [[SRC_TEMP]] : $*Error to HomeworkError in [[DEST_TEMP]] : $*HomeworkError, [[IS_HWE:bb[0-9]+]], [[NOT_HWE:bb[0-9]+]]

//   Catch HomeworkError.
// CHECK:    [[IS_HWE]]:
// CHECK-NEXT: [[T0_ORIG:%.*]] = load [take] [[DEST_TEMP]] : $*HomeworkError
// CHECK-NEXT: switch_enum [[T0_ORIG]] : $HomeworkError, case #HomeworkError.CatAteIt!enumelt: [[MATCH_ATE:bb[0-9]+]], case #HomeworkError.CatHidIt!enumelt: [[MATCH_HID:bb[0-9]+]], default [[NO_MATCH:bb[0-9]+]]

//   Catch HomeworkError.CatAteIt.
// CHECK:    [[MATCH_ATE]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: [[T0_COPY:%.*]] = copy_value [[T0]]
// CHECK-NEXT: destroy_value [[T0]]
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: br [[EXTRACT:bb[0-9]+]]([[T0_COPY]] : $Cat)

//   Catch HomeworkError.CatHidIt.
// CHECK:    [[MATCH_HID]]([[T0:%.*]] : @owned $Cat):
// CHECK-NEXT: [[T0_COPY:%.*]] = copy_value [[T0]]
// CHECK-NEXT: destroy_value [[T0]]
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: br [[EXTRACT]]([[T0_COPY]] : $Cat)

// CHECK:    [[EXTRACT]]([[CAT:%.*]] : @owned $Cat):
// CHECK-NEXT: debug_value [[CAT]] : $Cat, let, name "theCat"
// CHECK-NEXT: [[BORROWED_CAT:%.*]] = begin_borrow [[CAT]] : $Cat
// CHECK-NEXT: [[COPIED_CAT:%.*]] = copy_value [[BORROWED_CAT]] : $Cat
// CHECK-NEXT: end_borrow [[BORROWED_CAT]] : $Cat
// CHECK-NEXT: destroy_value [[CAT]] : $Cat
// CHECK-NEXT: destroy_value [[ERROR]] : $Error
// CHECK-NEXT: br [[RETURN]]([[COPIED_CAT]] : $Cat)

//   Catch other HomeworkErrors.
// CHECK:    [[NO_MATCH]]([[CATCHALL_ERROR:%.*]] : @owned $HomeworkError):
// CHECK-NEXT: destroy_value [[CATCHALL_ERROR]]
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: br [[RETHROW:bb[0-9]+]]

//   Catch other types.
// CHECK:    [[NOT_HWE]]:
// CHECK-NEXT: dealloc_stack [[DEST_TEMP]]
// CHECK-NEXT: destroy_addr [[SRC_TEMP]]
// CHECK-NEXT: dealloc_stack [[SRC_TEMP]]
// CHECK-NEXT: end_borrow [[BORROWED_ERROR]]
// CHECK-NEXT: br [[RETHROW]]

// Rethrow
// CHECK: [[RETHROW]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: throw [[ERROR]] : $Error
func all_together_now_five(_ flag: Bool) throws -> Cat {
  do {
    return try dont_return(Cat())
  } catch HomeworkError.CatAteIt(let theCat), HomeworkError.CatHidIt(let theCat) {
    return theCat
  }
}

//   Catch in non-throwing context.
// CHECK-LABEL: sil hidden [ossa] @$s6errors11catch_a_catAA3CatCyF : $@convention(thin) () -> @owned Cat
// CHECK-NEXT: bb0:
// CHECK-NEXT: [[M:%.*]] = metatype $@thick Cat.Type
// CHECK:      [[F:%.*]] = function_ref @$s6errors3Cat{{.*}} : $@convention(method) (@thick Cat.Type) -> @owned Cat
// CHECK-NEXT: [[V:%.*]] = apply [[F]]([[M]])
// CHECK-NEXT: return [[V]] : $Cat
func catch_a_cat() -> Cat {
  do {
    return Cat()
  } catch _ as HomeworkError {}  // expected-warning {{'catch' block is unreachable because no errors are thrown in 'do' block}}
}

// Initializers.
class HasThrowingInit {
  var field: Int
  init(value: Int) throws {
    field = value
  }
}

// CHECK-LABEL: sil hidden [exact_self_class] [ossa] @$s6errors15HasThrowingInit{{.*}} : $@convention(method) (Int, @thick HasThrowingInit.Type) -> (@owned HasThrowingInit, @error Error)
// CHECK:      [[SELF:%.*]] = alloc_ref $HasThrowingInit
// CHECK:      [[T0:%.*]] = function_ref @$s6errors15HasThrowingInit{{.*}}c : $@convention(method) (Int, @owned HasThrowingInit) -> (@owned HasThrowingInit, @error Error)
// CHECK-NEXT: try_apply [[T0]](%0, [[SELF]]) : $@convention(method) (Int, @owned HasThrowingInit) -> (@owned HasThrowingInit, @error Error), normal bb1, error bb2
// CHECK:    bb1([[SELF:%.*]] : @owned $HasThrowingInit):
// CHECK-NEXT: return [[SELF]]
// CHECK:    bb2([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: throw [[ERROR]]

// CHECK-LABEL: sil hidden [ossa] @$s6errors15HasThrowingInit{{.*}} : $@convention(method) (Int, @owned HasThrowingInit) -> (@owned HasThrowingInit, @error Error) {
// CHECK:      [[T0:%.*]] = mark_uninitialized [rootself] %1 : $HasThrowingInit
// CHECK-NEXT: [[BORROWED_T0:%.*]] = begin_borrow [[T0]]
// CHECK-NEXT: [[T1:%.*]] = ref_element_addr [[BORROWED_T0]] : $HasThrowingInit
// CHECK-NEXT: [[WRITE:%.*]] = begin_access [modify] [dynamic] [[T1]] : $*Int
// CHECK-NEXT: assign %0 to [[WRITE]] : $*Int
// CHECK-NEXT: end_access [[WRITE]]
// CHECK-NEXT: end_borrow [[BORROWED_T0]]
// CHECK-NEXT: [[T0_RET:%.*]] = copy_value [[T0]]
// CHECK-NEXT: destroy_value [[T0]]
// CHECK-NEXT: return [[T0_RET]] : $HasThrowingInit


enum ColorError : Error {
  case Red, Green, Blue
}

//CHECK-LABEL: sil hidden [ossa] @$s6errors6IThrows5Int32VyKF
//CHECK: builtin "willThrow"
//CHECK-NEXT: dealloc_stack
//CHECK-NEXT: throw
func IThrow() throws -> Int32 {
  throw ColorError.Red
  return 0  // expected-warning {{will never be executed}}
}

// Make sure that we are not emitting calls to 'willThrow' on rethrow sites.
//CHECK-LABEL: sil hidden [ossa] @$s6errors12DoesNotThrows5Int32VyKF
//CHECK-NOT: builtin "willThrow"
//CHECK: return
func DoesNotThrow() throws -> Int32 {
  _ = try IThrow()
  return 2
}

// rdar://20782111
protocol Doomed {
  func check() throws
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s6errors12DoomedStructVAA0B0A2aDP5checkyyKFTW :
// CHECK:      [[SELF:%.*]] = load [trivial] %0 : $*DoomedStruct
// CHECK:      [[T0:%.*]] = function_ref @$s6errors12DoomedStructV5checkyyKF : $@convention(method) (DoomedStruct) -> @error Error
// CHECK-NEXT: try_apply [[T0]]([[SELF]])
// CHECK:    bb1([[T0:%.*]] : $()):
// CHECK:      [[T0:%.*]] = tuple ()
// CHECK:      return [[T0]] : $()
// CHECK:    bb2([[T0:%.*]] : @owned $Error):
// CHECK:      throw [[T0]] : $Error
struct DoomedStruct : Doomed {
  func check() throws {}
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s6errors11DoomedClassCAA0B0A2aDP5checkyyKFTW :
// CHECK:      [[BORROWED_SELF:%.*]] = load_borrow %0
// CHECK:      [[T0:%.*]] = class_method [[BORROWED_SELF]] : $DoomedClass, #DoomedClass.check : (DoomedClass) -> () throws -> (), $@convention(method) (@guaranteed DoomedClass) -> @error Error
// CHECK-NEXT: try_apply [[T0]]([[BORROWED_SELF]])
// CHECK:    bb1([[T0:%.*]] : $()):
// CHECK:      [[T0:%.*]] = tuple ()
// CHECK:      end_borrow [[BORROWED_SELF]]
// CHECK:      return [[T0]] : $()
// CHECK:    bb2([[T0:%.*]] : @owned $Error):
// CHECK:      end_borrow [[BORROWED_SELF]]
// CHECK:      throw [[T0]] : $Error
class DoomedClass : Doomed {
  func check() throws {}
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s6errors11HappyStructVAA6DoomedA2aDP5checkyyKFTW :
// CHECK:      [[T0:%.*]] = function_ref @$s6errors11HappyStructV5checkyyF : $@convention(method) (HappyStruct) -> ()
// CHECK:      [[T1:%.*]] = apply [[T0]](%1)
// CHECK:      [[T1:%.*]] = tuple ()
// CHECK:      return [[T1]] : $()
struct HappyStruct : Doomed {
  func check() {}
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s6errors10HappyClassCAA6DoomedA2aDP5checkyyKFTW :
// CHECK:      [[SELF:%.*]] = load_borrow %0 : $*HappyClass
// CHECK:      [[T0:%.*]] = class_method [[SELF]] : $HappyClass, #HappyClass.check : (HappyClass) -> () -> (), $@convention(method) (@guaranteed HappyClass) -> ()
// CHECK:      [[T1:%.*]] = apply [[T0]]([[SELF]])
// CHECK:      [[T1:%.*]] = tuple ()
// CHECK:      end_borrow [[SELF]]
// CHECK:      return [[T1]] : $()
class HappyClass : Doomed {
  func check() {}
}

func create<T>(_ fn: () throws -> T) throws -> T {
  return try fn()
}
func testThunk(_ fn: () throws -> Int) throws -> Int {
  return try create(fn)
}
// CHECK-LABEL: sil shared [transparent] [serializable] [reabstraction_thunk] [ossa] @$sSis5Error_pIgdzo_SisAA_pIegrzo_TR : $@convention(thin) (@noescape @callee_guaranteed () -> (Int, @error Error)) -> (@out Int, @error Error)
// CHECK: bb0(%0 : $*Int, %1 : $@noescape @callee_guaranteed () -> (Int, @error Error)):
// CHECK:   try_apply %1()
// CHECK: bb1([[T0:%.*]] : $Int):
// CHECK:   store [[T0]] to [trivial] %0 : $*Int
// CHECK:   [[T0:%.*]] = tuple ()
// CHECK:   return [[T0]]
// CHECK: bb2([[T0:%.*]] : @owned $Error):
// CHECK:   throw [[T0]] : $Error

func createInt(_ fn: () -> Int) throws {}
func testForceTry(_ fn: () -> Int) {
  try! createInt(fn)
}
// CHECK-LABEL: sil hidden [ossa] @$s6errors12testForceTryyySiyXEF : $@convention(thin) (@noescape @callee_guaranteed () -> Int) -> ()
// CHECK: bb0([[ARG:%.*]] : $@noescape @callee_guaranteed () -> Int):
// CHECK: [[FUNC:%.*]] = function_ref @$s6errors9createIntyySiyXEKF : $@convention(thin) (@noescape @callee_guaranteed () -> Int) -> @error Error
// CHECK: try_apply [[FUNC]]([[ARG]])
// CHECK: return
// CHECK: function_ref @swift_unexpectedError
// CHECK: unreachable

func testForceTryMultiple() {
  _ = try! (make_a_cat(), make_a_cat())
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors20testForceTryMultipleyyF
// CHECK-NEXT: bb0:
// CHECK: [[FN_1:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN_1]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS_1:[^ ]+]], error [[CLEANUPS_1:[^ ]+]],
// CHECK: [[SUCCESS_1]]([[VALUE_1:%.+]] : @owned $Cat)
// CHECK: [[FN_2:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN_2]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS_2:[^ ]+]], error [[CLEANUPS_2:[^ ]+]],
// CHECK: [[SUCCESS_2]]([[VALUE_2:%.+]] : @owned $Cat)
// CHECK-NEXT: destroy_value [[VALUE_2]] : $Cat
// CHECK-NEXT: destroy_value [[VALUE_1]] : $Cat
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[FAILURE:.+]]([[ERROR:%.+]] : @owned $Error):
// CHECK: [[UNEXPECTED_ERROR:%.+]] = function_ref @swift_unexpectedError
// CHECK-NEXT: apply [[UNEXPECTED_ERROR]]([[ERROR]]
// CHECK-NEXT: unreachable
// CHECK: [[CLEANUPS_1]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: br [[FAILURE]]([[ERROR]] : $Error)
// CHECK: [[CLEANUPS_2]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: destroy_value [[VALUE_1]] : $Cat
// CHECK-NEXT: br [[FAILURE]]([[ERROR]] : $Error)
// CHECK: } // end sil function '$s6errors20testForceTryMultipleyyF'

// Make sure we balance scopes correctly inside a switch.
// <rdar://problem/20923654>
enum CatFood {
  case Canned
  case Dry
}

// Something we can switch on that throws.
func preferredFood() throws -> CatFood {
  return CatFood.Canned
}

func feedCat() throws -> Int {
  switch try preferredFood() {
  case .Canned:
    return 0
  case .Dry:
    return 1
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s6errors7feedCatSiyKF : $@convention(thin) () -> (Int, @error Error)
// CHECK: debug_value undef : $Error, var, name "$error", argno 1
// CHECK:   %1 = function_ref @$s6errors13preferredFoodAA03CatC0OyKF : $@convention(thin) () -> (CatFood, @error Error)
// CHECK:   try_apply %1() : $@convention(thin) () -> (CatFood, @error Error), normal bb1, error bb5
// CHECK: bb1([[VAL:%.*]] : $CatFood):
// CHECK:   switch_enum [[VAL]] : $CatFood, case #CatFood.Canned!enumelt: bb2, case #CatFood.Dry!enumelt: bb3
// CHECK: bb5([[ERROR:%.*]] : @owned $Error)
// CHECK:   throw [[ERROR]] : $Error

// Throwing statements inside cases.
func getHungryCat(_ food: CatFood) throws -> Cat {
  switch food {
  case .Canned:
    return try make_a_cat()
  case .Dry:
    return try dont_make_a_cat()
  }
}
// errors.getHungryCat throws (errors.CatFood) -> errors.Cat
// CHECK-LABEL: sil hidden [ossa] @$s6errors12getHungryCatyAA0D0CAA0D4FoodOKF : $@convention(thin) (CatFood) -> (@owned Cat, @error Error)
// CHECK: bb0(%0 : $CatFood):
// CHECK:   debug_value undef : $Error, var, name "$error", argno 2
// CHECK:   switch_enum %0 : $CatFood, case #CatFood.Canned!enumelt: bb1, case #CatFood.Dry!enumelt: bb3
// CHECK: bb1:
// CHECK:   [[FN:%.*]] = function_ref @$s6errors10make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK:   try_apply [[FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal bb2, error bb6
// CHECK: bb3:
// CHECK:   [[FN:%.*]] = function_ref @$s6errors15dont_make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK:   try_apply [[FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal bb4, error bb7
// CHECK: bb6([[ERROR:%.*]] : @owned $Error):
// CHECK:   br bb8([[ERROR:%.*]] : $Error)
// CHECK: bb7([[ERROR:%.*]] : @owned $Error):
// CHECK:   br bb8([[ERROR]] : $Error)
// CHECK: bb8([[ERROR:%.*]] : @owned $Error):
// CHECK:   throw [[ERROR]] : $Error

func take_many_cats(_ cats: Cat...) throws {}
func test_variadic(_ cat: Cat) throws {
  try take_many_cats(make_a_cat(), cat, make_a_cat(), make_a_cat())
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors13test_variadicyyAA3CatCKF : $@convention(thin) (@guaranteed Cat) -> @error Error {
// CHECK:       bb0([[ARG:%.*]] : @guaranteed $Cat):
// CHECK:         debug_value undef : $Error, var, name "$error", argno 2
// CHECK:         [[N:%.*]] = integer_literal $Builtin.Word, 4
// CHECK:         [[T0:%.*]] = function_ref @$ss27_allocateUninitializedArray{{.*}}F
// CHECK:         [[T1:%.*]] = apply [[T0]]<Cat>([[N]])
// CHECK:         ([[ARRAY:%.*]], [[T2:%.*]]) = destructure_tuple [[T1]]
// CHECK:         [[ELT0:%.*]] = pointer_to_address [[T2]] : $Builtin.RawPointer to [strict] $*Cat
//   Element 0.
// CHECK:         [[T0:%.*]] = function_ref @$s6errors10make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK:         try_apply [[T0]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[NORM_0:bb[0-9]+]], error [[ERR_0:bb[0-9]+]]
// CHECK:       [[NORM_0]]([[CAT0:%.*]] : @owned $Cat):
// CHECK-NEXT:    store [[CAT0]] to [init] [[ELT0]]
//   Element 1.
// CHECK-NEXT:    [[T0:%.*]] = integer_literal $Builtin.Word, 1
// CHECK-NEXT:    [[ELT1:%.*]] = index_addr [[ELT0]] : $*Cat, [[T0]]
// CHECK-NEXT:    [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK-NEXT:    store [[ARG_COPY]] to [init] [[ELT1]]
//   Element 2.
// CHECK-NEXT:    [[T0:%.*]] = integer_literal $Builtin.Word, 2
// CHECK-NEXT:    [[ELT2:%.*]] = index_addr [[ELT0]] : $*Cat, [[T0]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[T0:%.*]] = function_ref @$s6errors10make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK-NEXT:    try_apply [[T0]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[NORM_2:bb[0-9]+]], error [[ERR_2:bb[0-9]+]]
// CHECK:       [[NORM_2]]([[CAT2:%.*]] : @owned $Cat):
// CHECK-NEXT:    store [[CAT2]] to [init] [[ELT2]]
//   Element 3.
// CHECK-NEXT:    [[T0:%.*]] = integer_literal $Builtin.Word, 3
// CHECK-NEXT:    [[ELT3:%.*]] = index_addr [[ELT0]] : $*Cat, [[T0]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[T0:%.*]] = function_ref @$s6errors10make_a_catAA3CatCyKF : $@convention(thin) () -> (@owned Cat, @error Error)
// CHECK-NEXT:    try_apply [[T0]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[NORM_3:bb[0-9]+]], error [[ERR_3:bb[0-9]+]]
// CHECK:       [[NORM_3]]([[CAT3:%.*]] : @owned $Cat):
// CHECK-NEXT:    store [[CAT3]] to [init] [[ELT3]]
//   Complete the call and return.
// CHECK:         [[FIN_FN:%.*]] = function_ref @$ss27_finalizeUninitializedArrayySayxGABnlF
// CHECK:         [[FIN_ARRAY:%.*]] = apply [[FIN_FN]]<Cat>([[ARRAY]])
// CHECK:         [[TAKE_FN:%.*]] = function_ref @$s6errors14take_many_catsyyAA3CatCd_tKF : $@convention(thin) (@guaranteed Array<Cat>) -> @error Error
// CHECK-NEXT:    try_apply [[TAKE_FN]]([[FIN_ARRAY]]) : $@convention(thin) (@guaranteed Array<Cat>) -> @error Error, normal [[NORM_CALL:bb[0-9]+]], error [[ERR_CALL:bb[0-9]+]]
// CHECK:       [[NORM_CALL]]([[T0:%.*]] : $()):
// CHECK-NEXT:    destroy_value [[FIN_ARRAY]]
// CHECK-NEXT:    [[T0:%.*]] = tuple ()
// CHECK-NEXT:    return
//   Failure from element 0.
// CHECK:       [[ERR_0]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NOT:     end_borrow
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[T0:%.*]] = function_ref @$ss29_deallocateUninitializedArray{{.*}}F
// CHECK-NEXT:    apply [[T0]]<Cat>([[ARRAY]])
// CHECK-NEXT:    br [[RETHROW:.*]]([[ERROR]] : $Error)
//   Failure from element 2.
// CHECK:       [[ERR_2]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT:    destroy_addr [[ELT1]]
// CHECK-NEXT:    destroy_addr [[ELT0]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[T0:%.*]] = function_ref @$ss29_deallocateUninitializedArray{{.*}}F
// CHECK-NEXT:    apply [[T0]]<Cat>([[ARRAY]])
// CHECK-NEXT:    br [[RETHROW]]([[ERROR]] : $Error)
//   Failure from element 3.
// CHECK:       [[ERR_3]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT:    destroy_addr [[ELT2]]
// CHECK-NEXT:    destroy_addr [[ELT1]]
// CHECK-NEXT:    destroy_addr [[ELT0]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[T0:%.*]] = function_ref @$ss29_deallocateUninitializedArray{{.*}}F
// CHECK-NEXT:    apply [[T0]]<Cat>([[ARRAY]])
// CHECK-NEXT:    br [[RETHROW]]([[ERROR]] : $Error)
//   Failure from call.
// CHECK:       [[ERR_CALL]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT:    destroy_value [[FIN_ARRAY]]
// CHECK-NEXT:    br [[RETHROW]]([[ERROR]] : $Error)
//   Rethrow.
// CHECK:       [[RETHROW]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT:    throw [[ERROR]]
// CHECK: } // end sil function '$s6errors13test_variadicyyAA3CatCKF'

// rdar://20861374
// Clear out the self box before delegating.
class BaseThrowingInit : HasThrowingInit {
  var subField: Int
  init(value: Int, subField: Int) throws {
    self.subField = subField
    try super.init(value: value)
  }
}
// CHECK: sil hidden [ossa] @$s6errors16BaseThrowingInit{{.*}}c : $@convention(method) (Int, Int, @owned BaseThrowingInit) -> (@owned BaseThrowingInit, @error Error)
// CHECK:      [[BOX:%.*]] = alloc_box ${ var BaseThrowingInit }
// CHECK:      [[MARKED_BOX:%.*]] = mark_uninitialized [derivedself] [[BOX]]
// CHECK:      [[PB:%.*]] = project_box [[MARKED_BOX]]
//   Initialize subField.
// CHECK:      [[T0:%.*]] = load_borrow [[PB]]
// CHECK-NEXT: [[T1:%.*]] = ref_element_addr [[T0]] : $BaseThrowingInit, #BaseThrowingInit.subField
// CHECK-NEXT: [[WRITE:%.*]] = begin_access [modify] [dynamic] [[T1]] : $*Int
// CHECK-NEXT: assign %1 to [[WRITE]]
// CHECK-NEXT: end_access [[WRITE]]
// CHECK-NEXT: end_borrow [[T0]]
//   Super delegation.
// CHECK-NEXT: [[T0:%.*]] = load [take] [[PB]]
// CHECK-NEXT: [[T2:%.*]] = upcast [[T0]] : $BaseThrowingInit to $HasThrowingInit
// CHECK: [[T3:%[0-9]+]] = function_ref @$s6errors15HasThrowingInitC5valueACSi_tKcfc : $@convention(method) (Int, @owned HasThrowingInit) -> (@owned HasThrowingInit, @error Error)
// CHECK-NEXT: apply [[T3]](%0, [[T2]])

// Cleanups for writebacks.

protocol Supportable {
  mutating func support() throws
}
protocol Buildable {
  associatedtype Structure : Supportable
  var firstStructure: Structure { get set }
  subscript(name: String) -> Structure { get set }
}
func supportFirstStructure<B: Buildable>(_ b: inout B) throws {
  try b.firstStructure.support()
}
// CHECK-LABEL: sil hidden [ossa] @$s6errors21supportFirstStructure{{.*}}F : $@convention(thin) <B where B : Buildable> (@inout B) -> @error Error {
// CHECK: [[MODIFY:%.*]] = witness_method $B, #Buildable.firstStructure!modify :
// CHECK: ([[T1:%.*]], [[TOKEN:%.*]]) = begin_apply [[MODIFY]]<B>([[BASE:%[0-9]*]])
// CHECK: [[SUPPORT:%.*]] = witness_method $B.Structure, #Supportable.support :
// CHECK: try_apply [[SUPPORT]]<B.Structure>([[T1]]) : {{.*}}, normal [[BB_NORMAL:bb[0-9]+]], error [[BB_ERROR:bb[0-9]+]]

// CHECK: [[BB_NORMAL]]
// CHECK: end_apply [[TOKEN]]
// CHECK: return

// CHECK: [[BB_ERROR]]([[ERROR:%.*]] : @owned $Error):
// CHECK: abort_apply [[TOKEN]]
// CHECK: throw [[ERROR]]

// CHECK: } // end sil function '$s6errors21supportFirstStructure{{.*}}F'

func supportStructure<B: Buildable>(_ b: inout B, name: String) throws {
  try b[name].support()
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors16supportStructure_4nameyxz_SStKAA9BuildableRzlF : $@convention(thin) <B where B : Buildable> (@inout B, @guaranteed String) -> @error Error {
// CHECK: bb0({{.*}}, [[INDEX:%.*]] : @guaranteed $String):
// CHECK:   [[INDEX_COPY:%.*]] = copy_value [[INDEX]] : $String
// CHECK:   [[BORROWED_INDEX_COPY:%.*]] = begin_borrow [[INDEX_COPY]]
// CHECK:   [[MODIFY:%.*]] = witness_method $B, #Buildable.subscript!modify :
// CHECK:   ([[T1:%.*]], [[TOKEN:%.*]]) = begin_apply [[MODIFY]]<B>([[BORROWED_INDEX_COPY]], [[BASE:%[0-9]*]])
// CHECK:   [[SUPPORT:%.*]] = witness_method $B.Structure, #Supportable.support :
// CHECK:   try_apply [[SUPPORT]]<B.Structure>([[T1]]) : $@convention(witness_method: Supportable) <τ_0_0 where τ_0_0 : Supportable> (@inout τ_0_0) -> @error Error, normal [[BB_NORMAL:bb[0-9]+]], error [[BB_ERROR:bb[0-9]+]]

// CHECK: [[BB_NORMAL]]
// CHECK:   end_apply [[TOKEN]]
// CHECK:   end_borrow [[BORROWED_INDEX_COPY]]
// CHECK:   destroy_value [[INDEX_COPY]] : $String
// CHECK:   return

// CHECK: [[BB_ERROR]]([[ERROR:%.*]] : @owned $Error):
// CHECK:   abort_apply [[TOKEN]]
// CHECK:   end_borrow [[BORROWED_INDEX_COPY]]
// CHECK:   destroy_value [[INDEX_COPY]] : $String
// CHECK:   throw [[ERROR]]

// CHECK: } // end sil function '$s6errors16supportStructure{{.*}}F'

struct Pylon {
  var name: String
  mutating func support() throws {}
}
struct Bridge {
  var mainPylon : Pylon
  subscript(name: String) -> Pylon {
    get {
      return mainPylon
    }
    set {}
  }
}
func supportStructure(_ b: inout Bridge, name: String) throws {
  try b[name].support()
}
// CHECK:    sil hidden [ossa] @$s6errors16supportStructure_4nameyAA6BridgeVz_SStKF : $@convention(thin) (@inout Bridge, @guaranteed String) -> @error Error {
// CHECK:    bb0([[ARG1:%.*]] : $*Bridge, [[ARG2:%.*]] : @guaranteed $String):
// CHECK:      [[INDEX_COPY_1:%.*]] = copy_value [[ARG2]] : $String
// CHECK-NEXT: [[WRITE:%.*]] = begin_access [modify] [unknown] [[ARG1]] : $*Bridge
// CHECK-NEXT: [[INDEX_COPY_2:%.*]] = copy_value [[INDEX_COPY_1]] : $String
// CHECK-NEXT: [[TEMP:%.*]] = alloc_stack $Pylon
// CHECK-NEXT: [[BASE:%.*]] = load_borrow [[WRITE]] : $*Bridge
// CHECK-NEXT: // function_ref
// CHECK-NEXT: [[GETTER:%.*]] = function_ref @$s6errors6BridgeVyAA5PylonVSScig :
// CHECK-NEXT: [[T0:%.*]] = apply [[GETTER]]([[INDEX_COPY_1]], [[BASE]])
// CHECK-NEXT: store [[T0]] to [init] [[TEMP]]
// CHECK-NEXT: end_borrow [[BASE]]
// CHECK:      [[SUPPORT:%.*]] = function_ref @$s6errors5PylonV7supportyyKF
// CHECK-NEXT: try_apply [[SUPPORT]]([[TEMP]]) : {{.*}}, normal [[BB_NORMAL:bb[0-9]+]], error [[BB_ERROR:bb[0-9]+]]

// CHECK:    [[BB_NORMAL]]
// CHECK-NEXT: [[T0:%.*]] = load [take] [[TEMP]]
// CHECK-NEXT: // function_ref
// CHECK-NEXT: [[SETTER:%.*]] = function_ref @$s6errors6BridgeVyAA5PylonVSScis :
// CHECK-NEXT: apply [[SETTER]]([[T0]], [[INDEX_COPY_2]], [[WRITE]])
// CHECK-NEXT: end_access [[WRITE]]
// CHECK-NEXT: dealloc_stack [[TEMP]]
// CHECK-NEXT: destroy_value [[INDEX_COPY_1]] : $String
// CHECK-NEXT: tuple ()
// CHECK-NEXT: return

//   We end up with ugly redundancy here because we don't want to
//   consume things during cleanup emission.  It's questionable.
// CHECK:    [[BB_ERROR]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: [[T0:%.*]] = load [copy] [[TEMP]]
// CHECK-NEXT: [[INDEX_COPY_2_COPY:%.*]] = copy_value [[INDEX_COPY_2]]
// CHECK-NEXT: // function_ref
// CHECK-NEXT: [[SETTER:%.*]] = function_ref @$s6errors6BridgeVyAA5PylonVSScis :
// CHECK-NEXT: apply [[SETTER]]([[T0]], [[INDEX_COPY_2_COPY]], [[WRITE]])
// CHECK-NEXT: destroy_addr [[TEMP]]
// CHECK-NEXT: dealloc_stack [[TEMP]]
// ==> SEMANTIC ARC TODO: INDEX_COPY_2 on the next line should be INDEX_COPY_2_COPY
// CHECK-NEXT: destroy_value [[INDEX_COPY_2]] : $String
// CHECK-NEXT: end_access [[WRITE]]
// CHECK-NEXT: destroy_value [[INDEX_COPY_1]] : $String
// CHECK-NEXT: throw [[ERROR]]
// CHECK: } // end sil function '$s6errors16supportStructure_4nameyAA6BridgeVz_SStKF'

// ! peepholes its argument with getSemanticsProvidingExpr().
// Test that that doesn't look through try!.
// rdar://21515402
func testForcePeephole(_ f: () throws -> Int?) -> Int {
  let x = (try! f())!
  return x
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors15testOptionalTryyyF
// CHECK-NEXT: bb0:
// CHECK: [[FN:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS:[^ ]+]], error [[CLEANUPS:[^ ]+]],
// CHECK: [[SUCCESS]]([[VALUE:%.+]] : @owned $Cat)
// CHECK-NEXT: [[WRAPPED:%.+]] = enum $Optional<Cat>, #Optional.some!enumelt, [[VALUE]]
// CHECK-NEXT: br [[DONE:[^ ]+]]([[WRAPPED]] : $Optional<Cat>)
// CHECK: [[DONE]]([[RESULT:%.+]] : @owned $Optional<Cat>):
// CHECK-NEXT: destroy_value [[RESULT]] : $Optional<Cat>
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[CLEANUPS:.+]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: [[NONE:%.+]] = enum $Optional<Cat>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE]]([[NONE]] : $Optional<Cat>)
// CHECK: } // end sil function '$s6errors15testOptionalTryyyF'
func testOptionalTry() {
  _ = try? make_a_cat()
}

func sudo_make_a_cat() {}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}testOptionalTryThatNeverThrows
func testOptionalTryThatNeverThrows() {
  guard let _ = try? sudo_make_a_cat() else { // expected-warning{{no calls to throwing}}
    return
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors18testOptionalTryVaryyF
// CHECK-NEXT: bb0:
// CHECK-NEXT: [[BOX:%.+]] = alloc_box ${ var Optional<Cat> }
// CHECK-NEXT: [[PB:%.*]] = project_box [[BOX]]
// CHECK: [[FN:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS:[^ ]+]], error [[CLEANUPS:[^ ]+]],
// CHECK: [[SUCCESS]]([[VALUE:%.+]] : @owned $Cat)
// CHECK-NEXT: [[CAT_ENUM:%.+]] = enum $Optional<Cat>, #Optional.some!enumelt
// CHECK-NEXT: store [[CAT_ENUM]] to [init] [[PB]] : $*Optional<Cat>
// CHECK-NEXT: br [[DONE:[^ ]+]],
// CHECK: [[DONE]]:
// CHECK-NEXT: destroy_value [[BOX]] : ${ var Optional<Cat> }
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[CLEANUPS:.+]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: inject_enum_addr [[PB]] : $*Optional<Cat>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE]]
// CHECK: } // end sil function '$s6errors18testOptionalTryVaryyF'
func testOptionalTryVar() {
  var cat = try? make_a_cat()
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors26testOptionalTryAddressOnly{{.*}}F
// CHECK: bb0(%0 : $*T):
// CHECK: [[BOX:%.+]] = alloc_stack $Optional<T>
// CHECK-NEXT: [[BOX_DATA:%.+]] = init_enum_data_addr [[BOX]] : $*Optional<T>, #Optional.some!enumelt
// CHECK: [[FN:%.+]] = function_ref @$s6errors11dont_return{{.*}}F
// CHECK-NEXT: try_apply [[FN]]<T>([[BOX_DATA]], %0) : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @error Error), normal [[SUCCESS:[^ ]+]], error [[CLEANUPS:[^ ]+]],
// CHECK: [[SUCCESS]]({{%.+}} : $()):
// CHECK-NEXT: inject_enum_addr [[BOX]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT: br [[DONE:[^ ]+]],
// CHECK: [[DONE]]:
// CHECK-NEXT: destroy_addr [[BOX]] : $*Optional<T>
// CHECK-NEXT: dealloc_stack [[BOX]] : $*Optional<T>
// CHECK-NOT: destroy_addr %0 : $*T
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[CLEANUPS]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: inject_enum_addr [[BOX]] : $*Optional<T>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE]]
// CHECK: } // end sil function '$s6errors26testOptionalTryAddressOnlyyyxlF'
func testOptionalTryAddressOnly<T>(_ obj: T) {
  _ = try? dont_return(obj)
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors29testOptionalTryAddressOnlyVar{{.*}}F
// CHECK: bb0(%0 : $*T):
// CHECK: [[BOX:%.+]] = alloc_box $<τ_0_0> { var Optional<τ_0_0> } <T>
// CHECK-NEXT: [[PB:%.*]] = project_box [[BOX]]
// CHECK-NEXT: [[BOX_DATA:%.+]] = init_enum_data_addr [[PB]] : $*Optional<T>, #Optional.some!enumelt
// CHECK: [[FN:%.+]] = function_ref @$s6errors11dont_return{{.*}}F
// CHECK-NEXT: try_apply [[FN]]<T>([[BOX_DATA]], %0) : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> (@out τ_0_0, @error Error), normal [[SUCCESS:[^ ]+]], error [[CLEANUPS:[^ ]+]],
// CHECK: [[SUCCESS]]({{%.+}} : $()):
// CHECK-NEXT: inject_enum_addr [[PB]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT: br [[DONE:[^ ]+]],
// CHECK: [[DONE]]:
// CHECK-NEXT: destroy_value [[BOX]] : $<τ_0_0> { var Optional<τ_0_0> } <T>
// CHECK-NOT: destroy_addr %0 : $*T
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[CLEANUPS]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: inject_enum_addr [[PB]] : $*Optional<T>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE]]
// CHECK: } // end sil function '$s6errors29testOptionalTryAddressOnlyVaryyxlF'
func testOptionalTryAddressOnlyVar<T>(_ obj: T) {
  var copy = try? dont_return(obj)
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors23testOptionalTryMultipleyyF
// CHECK: bb0:
// CHECK: [[FN_1:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN_1]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS_1:[^ ]+]], error [[CLEANUPS_1:[^ ]+]],
// CHECK: [[SUCCESS_1]]([[VALUE_1:%.+]] : @owned $Cat)
// CHECK: [[FN_2:%.+]] = function_ref @$s6errors10make_a_catAA3CatCyKF
// CHECK-NEXT: try_apply [[FN_2]]() : $@convention(thin) () -> (@owned Cat, @error Error), normal [[SUCCESS_2:[^ ]+]], error [[CLEANUPS_2:[^ ]+]],
// CHECK: [[SUCCESS_2]]([[VALUE_2:%.+]] : @owned $Cat)
// CHECK-NEXT: [[TUPLE:%.+]] = tuple ([[VALUE_1]] : $Cat, [[VALUE_2]] : $Cat)
// CHECK-NEXT: [[WRAPPED:%.+]] = enum $Optional<(Cat, Cat)>, #Optional.some!enumelt, [[TUPLE]]
// CHECK-NEXT: br [[DONE:[^ ]+]]([[WRAPPED]] : $Optional<(Cat, Cat)>)
// CHECK: [[DONE]]([[RESULT:%.+]] : @owned $Optional<(Cat, Cat)>):
// CHECK-NEXT: destroy_value [[RESULT]] : $Optional<(Cat, Cat)>
// CHECK-NEXT: [[VOID:%.+]] = tuple ()
// CHECK-NEXT: return [[VOID]] : $()
// CHECK: [[FAILURE:.+]]([[ERROR:%.*]] : @owned $Error):
// CHECK-NEXT: destroy_value [[ERROR]]
// CHECK-NEXT: [[NONE:%.+]] = enum $Optional<(Cat, Cat)>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE]]([[NONE]] : $Optional<(Cat, Cat)>)
// CHECK: [[CLEANUPS_1]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: br [[FAILURE]]([[ERROR]] : $Error)
// CHECK: [[CLEANUPS_2]]([[ERROR:%.+]] : @owned $Error):
// CHECK-NEXT: destroy_value [[VALUE_1]] : $Cat
// CHECK-NEXT: br [[FAILURE]]([[ERROR]] : $Error)
// CHECK: } // end sil function '$s6errors23testOptionalTryMultipleyyF'
func testOptionalTryMultiple() {
  _ = try? (make_a_cat(), make_a_cat())
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors25testOptionalTryNeverFailsyyF
// CHECK: bb0:
// CHECK-NEXT:   [[VALUE:%.+]] = tuple ()
// CHECK-NEXT:   = enum $Optional<()>, #Optional.some!enumelt, [[VALUE]]
// CHECK-NEXT:   [[VOID:%.+]] = tuple ()
// CHECK-NEXT:   return [[VOID]] : $()
// CHECK: } // end sil function '$s6errors25testOptionalTryNeverFailsyyF'
func testOptionalTryNeverFails() {
  _ = try? () // expected-warning {{no calls to throwing functions occur within 'try' expression}}
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors28testOptionalTryNeverFailsVaryyF
// CHECK: bb0:
// CHECK-NEXT:   [[BOX:%.+]] = alloc_box ${ var Optional<()> }
// CHECK-NEXT:   [[PB:%.*]] = project_box [[BOX]]
// CHECK-NEXT:   [[VALUE:%.+]] = tuple ()
// CHECK-NEXT:   [[ENUM:%.+]] = enum $Optional<()>, #Optional.some!enumelt, [[VALUE]]
// CHECK-NEXT:   store [[ENUM]] to [trivial] [[PB]] :
// CHECK-NEXT:   destroy_value [[BOX]] : ${ var Optional<()> }
// CHECK-NEXT:   [[VOID:%.+]] = tuple ()
// CHECK-NEXT:   return [[VOID]] : $()
// CHECK-NEXT: } // end sil function '$s6errors28testOptionalTryNeverFailsVaryyF'
func testOptionalTryNeverFailsVar() {
  var unit: ()? = try? () // expected-warning {{no calls to throwing functions occur within 'try' expression}}
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors36testOptionalTryNeverFailsAddressOnly{{.*}}F
// CHECK: bb0(%0 : $*T):
// CHECK:   [[BOX:%.+]] = alloc_stack $Optional<T>
// CHECK-NEXT:   [[BOX_DATA:%.+]] = init_enum_data_addr [[BOX]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT:   copy_addr %0 to [initialization] [[BOX_DATA]] : $*T
// CHECK-NEXT:   inject_enum_addr [[BOX]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT:   destroy_addr [[BOX]] : $*Optional<T>
// CHECK-NEXT:   dealloc_stack [[BOX]] : $*Optional<T>
// CHECK-NOT:   destroy_addr %0 : $*T
// CHECK-NEXT:   [[VOID:%.+]] = tuple ()
// CHECK-NEXT:   return [[VOID]] : $()
// CHECK-NEXT: } // end sil function '$s6errors36testOptionalTryNeverFailsAddressOnlyyyxlF'
func testOptionalTryNeverFailsAddressOnly<T>(_ obj: T) {
  _ = try? obj // expected-warning {{no calls to throwing functions occur within 'try' expression}}
}

// CHECK-LABEL: sil hidden [ossa] @$s6errors39testOptionalTryNeverFailsAddressOnlyVar{{.*}}F
// CHECK: bb0(%0 : $*T):
// CHECK:   [[BOX:%.+]] = alloc_box $<τ_0_0> { var Optional<τ_0_0> } <T>
// CHECK-NEXT:   [[PB:%.*]] = project_box [[BOX]]
// CHECK-NEXT:   [[BOX_DATA:%.+]] = init_enum_data_addr [[PB]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT:   copy_addr %0 to [initialization] [[BOX_DATA]] : $*T
// CHECK-NEXT:   inject_enum_addr [[PB]] : $*Optional<T>, #Optional.some!enumelt
// CHECK-NEXT:   destroy_value [[BOX]] : $<τ_0_0> { var Optional<τ_0_0> } <T>
// CHECK-NEXT:   [[VOID:%.+]] = tuple ()
// CHECK-NEXT:   return [[VOID]] : $()
// CHECK: } // end sil function '$s6errors13OtherErrorSubCACycfC'
func testOptionalTryNeverFailsAddressOnlyVar<T>(_ obj: T) {
  var copy = try? obj // expected-warning {{no calls to throwing functions occur within 'try' expression}}
}

class SomeErrorClass : Error { }

// CHECK-LABEL: sil_vtable SomeErrorClass
// CHECK-NEXT:   #SomeErrorClass.init!allocator: {{.*}} : @$s6errors14SomeErrorClassCACycfC
// CHECK-NEXT:   #SomeErrorClass.deinit!deallocator: @$s6errors14SomeErrorClassCfD
// CHECK-NEXT: }

class OtherErrorSub : OtherError { }

// CHECK-LABEL: sil_vtable OtherErrorSub {
// CHECK-NEXT:  #OtherError.init!allocator: {{.*}} : @$s6errors13OtherErrorSubCACycfC [override]
// CHECK-NEXT:  #OtherErrorSub.deinit!deallocator: @$s6errors13OtherErrorSubCfD        // OtherErrorSub.__deallocating_deinit
// CHECK-NEXT:}
