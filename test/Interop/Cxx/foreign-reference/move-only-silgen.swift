// RUN: %target-swift-emit-silgen %s -I %S/Inputs -enable-experimental-cxx-interop -disable-availability-checking | %FileCheck %s

import MoveOnly

// CHECK-NOT: borrow
// CHECK-NOT: retain
// CHECK-NOT: release
// CHECK-LABEL: sil [ossa] @$s4main4testyyF : $@convention(thin) () -> ()

// CHECK: [[BOX:%.*]] = project_box {{.*}} : ${ var MoveOnly }, 0

// CHECK: [[CREATE_FN:%.*]] = function_ref @{{_ZN8MoveOnly6createEv|\?create\@MoveOnly\@\@SAPEAU1\@XZ}} : $@convention(c) () -> MoveOnly
// CHECK: [[CREATED_PTR:%.*]] = apply [[CREATE_FN]]() : $@convention(c) () -> MoveOnly
// CHECK: store [[CREATED_PTR]] to [trivial] [[BOX]] : $*MoveOnly
// CHECK: [[ACCESS_1:%.*]] = begin_access [read] [unknown] [[BOX]] : $*MoveOnly
// CHECK: [[X_1:%.*]] = load [trivial] [[ACCESS_1]] : $*MoveOnly

// CHECK: [[TMP:%.*]] = alloc_stack $MoveOnly
// CHECK: store [[X_1]] to [trivial] [[TMP]]

// CHECK: [[TEST_FN:%.*]] = function_ref @{{_ZNK8MoveOnly4testEv|\?test\@MoveOnly\@\@QEBAHXZ}} : $@convention(cxx_method) (@in_guaranteed MoveOnly) -> Int32
// CHECK: apply [[TEST_FN]]([[TMP]]) : $@convention(cxx_method) (@in_guaranteed MoveOnly) -> Int32

// CHECK: return
// CHECK-LABEL: end sil function '$s4main4testyyF'
public func test() {
  var x = MoveOnly.create()
  _ = x.test()
}

// CHECK-LABEL: sil{{ \[available .*\] | }}[clang MoveOnly.create] @{{_ZN8MoveOnly6createEv|\?create\@MoveOnly\@\@SAPEAU1\@XZ}} : $@convention(c) () -> MoveOnly

// CHECK-LABEL: sil{{ \[available .*\] | }}[clang MoveOnly.test] @{{_ZNK8MoveOnly4testEv|\?test\@MoveOnly\@\@QEBAHXZ}} : $@convention(cxx_method) (@in_guaranteed MoveOnly) -> Int32
