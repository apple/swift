// RUN: %target-swift-frontend %/s -parse-as-library  -enable-spec-devirt -O -emit-sil -save-optimization-record-path %t.opt.yaml | %FileCheck %s
// RUN: %FileCheck -check-prefix=YAML -input-file=%t.opt.yaml %s
// RUN: %target-swift-frontend %/s -parse-as-library -Osize -emit-sil | %FileCheck %s --check-prefix=OSIZE
//
// Test speculative devirtualization.

// Test MaxNumSpeculativeTargets.
// rdar:23228386
public class Base {
  public init() {}
  public func foo() {}
}

@_optimize(none)
func blackHole<T : AnyObject>(_: T) {}

class Sub1 : Base {
  override func foo() { blackHole(self) }
}
class Sub2 : Base {
  override func foo() { blackHole(self) }
}
class Sub3 : Base {
  override func foo() { blackHole(self) }
}
class Sub4 : Base {
  override func foo() { blackHole(self) }
}
class Sub5 : Base {
  override func foo() { blackHole(self) }
}
class Sub6 : Base {
  override func foo() { blackHole(self) }
}
class Sub7 : Base {
  override func foo() { blackHole(self) }
}
// CHECK: @$s16devirt_speculate28testMaxNumSpeculativeTargetsyyAA4BaseCF
// CHECK: bb0(%0 : $Base):
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Base, bb2, bb3

// CHECK: bb2([[CASTED:%.*]]):
// CHECK:   br bb1

// CHECK: bb3:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub1, bb4, bb5

// CHECK: bb4([[CASTED:%.*]] : $Sub1):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub1>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb5:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub2, bb6, bb7

// CHECK: bb6([[CASTED:%.*]] : $Sub2):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub2>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb7:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub3, bb8, bb9

// CHECK: bb8([[CASTED:%.*]] : $Sub3):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub3>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb9:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub4, bb10, bb11

// CHECK: bb10([[CASTED:%.*]] : $Sub4):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub4>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb11:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub5, bb12, bb13

// CHECK: bb12([[CASTED:%.*]] : $Sub5):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub5>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb13:
// CHECK:   checked_cast_br [exact] Base in %0 : $Base to Sub6, bb14, bb15

// CHECK: bb14([[CASTED:%.*]] : $Sub6):
// CHECK:   [[FN:%.*]] = function_ref @$s16devirt_speculate9blackHoleyyxRlzClF : $@convention(thin) <τ_0_0 where τ_0_0 : AnyObject> (@guaranteed τ_0_0) -> ()
// CHECK:   apply [[FN]]<Sub6>([[CASTED]])
// CHECK:   br bb1

// CHECK: bb15:
// CHECK-NOT: checked_cast_br
// CHECK:   %[[CM:[0-9]+]] = class_method %0 : $Base, #Base.foo : (Base) -> () -> (), $@convention(method) (@guaranteed Base) -> ()
// CHECK:   apply %[[CM]](%0) : $@convention(method) (@guaranteed Base) -> ()

// YAML:      Pass:            sil-speculative-devirtualizer
// YAML-NEXT: Name:            sil.PartialSpecDevirt
// YAML-NEXT: DebugLoc:
// YAML:   File:            {{.*}}/devirt_speculate.swift
// YAML:   Line:            118
// YAML:   Column:          5
// YAML-NEXT: Function:        'testMaxNumSpeculativeTargets(_:)'
// YAML-NEXT: Args:
// YAML-NEXT:   - String:          'Partially devirtualized call with run-time checks for '
// YAML-NEXT:   - NumSubTypesChecked: '6'
// YAML-NEXT:   - String:          ' subclasses of '
// YAML-NEXT:   - ClassType:       Base
// YAML-NEXT:   - String:          ', number of subclasses not devirtualized: '
// YAML-NEXT:   - NotHandledSubsNum: '1'
// YAML-NEXT: ...

// OSIZE: @$s16devirt_speculate28testMaxNumSpeculativeTargetsyyAA4BaseCF
// OSIZE-NOT: checked_cast_br [exact] Base in %0 : $Base to Base
// OSIZE-NOT: checked_cast_br [exact] Base in %0 : $Base to Sub
public func testMaxNumSpeculativeTargets(_ b: Base) {
  b.foo()
}
