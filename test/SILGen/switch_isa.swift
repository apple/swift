
// RUN: %target-swift-emit-silgen -module-name switch_isa %s | %FileCheck %s

func markUsed<T>(_ t: T) {}

// rdar://17772217
func testSwitchOnExistential(_ value: Any) {
  switch value {
    case true as Bool: markUsed("true")
    case false as Bool: markUsed("false")
    default: markUsed("default")
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_isa23testSwitchOnExistentialyyypF :
// CHECK:   [[ANY:%.*]] = alloc_stack $Any
// CHECK:   copy_addr %0 to [init] [[ANY]]
// CHECK:   [[BOOL:%.*]] = alloc_stack $Bool
// CHECK:   checked_cast_addr_br copy_on_success Any in [[ANY]] : $*Any to Bool in [[BOOL]] : $*Bool, [[IS_BOOL:bb[0-9]+]], [[IS_NOT_BOOL:bb[0-9]+]]
// CHECK: [[IS_BOOL]]:
// CHECK:   [[T0:%.*]] = load [trivial] [[BOOL]]

enum Foo {
  case A
}
enum Bar<T> {
  case B(T)
}
func testSwitchEnumOnExistential(_ value: Any) {
  switch value {
  case Foo.A:
    ()
  case Bar<Int>.B(let i):
    ()
  case Bar<Foo>.B(let f):
    ()
  default:
    ()
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s10switch_isa27testSwitchEnumOnExistentialyyypF : $@convention(thin) (@in_guaranteed Any) -> ()
// CHECK:   checked_cast_addr_br copy_on_success Any in {{%.*}} : $*Any to Foo
// CHECK:   checked_cast_addr_br copy_on_success Any in {{%.*}} : $*Any to Bar<Int>
// CHECK:   checked_cast_addr_br copy_on_success Any in {{%.*}} : $*Any to Bar<Foo>

class B {}
class D: B {}

func guardFn(_ l: D, _ r: D) -> Bool { return true }

// rdar://problem/21087371
// CHECK-LABEL: sil hidden [ossa] @$s10switch_isa32testSwitchTwoIsPatternsWithGuard_1ryAA1BC_AEtF
// CHECK: bb0([[ARG0:%.*]] : @guaranteed $B, [[ARG1:%.*]] : @guaranteed $B):
// CHECK:       [[ARG0_COPY:%.*]] = copy_value [[ARG0]]
// CHECK:       [[ARG1_COPY:%.*]] = copy_value [[ARG1]]
// CHECK:       [[TUP:%.*]] = tuple ([[ARG0_COPY:%.*]] : $B, [[ARG1_COPY:%.*]] : $B)
// CHECK:       [[BORROWED_TUP:%.*]] = begin_borrow [[TUP]]
// CHECK:       ([[TUP_1:%.*]], [[TUP_2:%.*]]) = destructure_tuple [[BORROWED_TUP]]
// CHECK:       checked_cast_br B in [[TUP_1]] : $B to D, [[R_CAST_YES:bb[0-9]+]], [[R_CAST_NO:bb[0-9]+]]
//
// CHECK:       [[R_CAST_YES]]([[R:%.*]] : @guaranteed $D):
// CHECK:         [[R2:%.*]] = copy_value [[R]]
// CHECK:         checked_cast_br B in [[TUP_2]] : $B to D, [[L_CAST_YES:bb[0-9]+]], [[L_CAST_NO:bb[0-9]+]]
//
// CHECK:       [[L_CAST_YES]]([[L:%.*]] : @guaranteed $D):
// CHECK:         [[L2:%.*]] = copy_value [[L]]
// CHECK:         [[BORROWED_R2:%.*]] = begin_borrow [lexical] [[R2]]
// CHECK:         [[BORROWED_L2:%.*]] = begin_borrow [lexical] [[L2]]
// CHECK:         function_ref @$s10switch_isa7guardFnySbAA1DC_ADtF
// CHECK:         cond_br {{%.*}}, [[GUARD_YES:bb[0-9]+]], [[GUARD_NO:bb[0-9]+]]
//
// CHECK:       [[GUARD_YES]]:
// CHECK-NEXT:    debug_value [[BORROWED_R2]]
// CHECK-NEXT:    debug_value [[BORROWED_L2]]
// CHECK-NEXT:    end_borrow [[BORROWED_L2]]
// CHECK-NEXT:    destroy_value [[L2]]
// CHECK-NEXT:    end_borrow [[BORROWED_R2]]
// CHECK-NEXT:    destroy_value [[R2]]
// CHECK-NEXT:    end_borrow [[BORROWED_TUP]]
// CHECK-NEXT:    destroy_value [[TUP]]
// CHECK-NEXT:    br [[EXIT:bb[0-9]+]]
//
// CHECK:       [[GUARD_NO]]:
// CHECK-NEXT:    end_borrow [[BORROWED_L2]]
// CHECK-NEXT:    destroy_value [[L2]]
// CHECK-NEXT:    end_borrow [[BORROWED_R2]]
// CHECK-NEXT:    destroy_value [[R2]]
// CHECK-NEXT:    end_borrow [[BORROWED_TUP]]
// CHECK-NEXT:    br [[CONT:bb[0-9]+]]
//
// CHECK:       [[L_CAST_NO]]([[LFAIL:%.*]] : @guaranteed $B):
// CHECK-NEXT:    destroy_value [[R2]]
// CHECK-NEXT:    end_borrow [[BORROWED_TUP]]
// CHECK-NEXT:    br [[CONT]]
//
// CHECK:       [[CONT]]:
// CHECK-NEXT:    destroy_value [[TUP]]
// CHECK-NEXT:    br [[EXIT]]
func testSwitchTwoIsPatternsWithGuard(_ l: B, r: B) {
  switch (l, r) {
  case (let l2 as D, let r2 as D) where guardFn(l2, r2):
    break
  default:
    break
  }
}
