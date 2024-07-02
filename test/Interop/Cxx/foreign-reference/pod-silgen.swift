// RUN: %target-swift-emit-silgen %s -I %S/Inputs -enable-experimental-cxx-interop -disable-availability-checking | %FileCheck %s

import POD

// CHECK-NOT: borrow
// CHECK-NOT: retain
// CHECK-NOT: release
// CHECK-LABEL: sil [ossa] @$s4main4testyyF : $@convention(thin) () -> ()

// CHECK: [[BOX:%.*]] = project_box {{.*}} : ${ var IntPair }, 0

// CHECK: [[CREATE_FN:%.*]] = function_ref @{{_ZN7IntPair6createEv|\?create\@IntPair\@\@SAPEAU1\@XZ}} : $@convention(c) () -> IntPair
// CHECK: [[CREATED_PTR:%.*]] = apply [[CREATE_FN]]() : $@convention(c) () -> IntPair
// CHECK: store [[CREATED_PTR]] to [trivial] [[BOX]] : $*IntPair
// CHECK: [[ACCESS_1:%.*]] = begin_access [read] [unknown] [[BOX]] : $*IntPair
// CHECK: [[X_1:%.*]] = load [trivial] [[ACCESS_1]] : $*IntPair

// CHECK: [[X_B:%.*]] = ref_element_addr [[X_1]] : $IntPair, #IntPair.b
// CHECK: [[ACCESS_2:%.*]] = begin_access [modify] [dynamic] [[X_B]] : $*Int32
// CHECK: assign {{.*}} to [[ACCESS_2]] : $*Int32

// CHECK: [[ACCESS_3:%.*]] = begin_access [read] [unknown] [[BOX]] : $*IntPair
// CHECK: [[X_2:%.*]] = load [trivial] [[ACCESS_3]] : $*IntPair
// CHECK: [[TMP:%.*]] = alloc_stack $IntPair
// CHECK: store [[X_2]] to [trivial] [[TMP]]
// CHECK: [[TEST_FN:%.*]] = function_ref @{{_ZNK7IntPair4testEv|\?test\@IntPair\@\@QEBAHXZ}} : $@convention(cxx_method) (@in_guaranteed IntPair) -> Int32
// CHECK: apply [[TEST_FN]]([[TMP]]) : $@convention(cxx_method) (@in_guaranteed IntPair) -> Int32

// CHECK: return
// CHECK-LABEL: end sil function '$s4main4testyyF'
public func test() {
  var x = IntPair.create()
  x.b = 42
  _ = x.test()
}

// CHECK-LABEL: sil{{ \[available .*\] | }}[clang IntPair.create] @{{_ZN7IntPair6createEv|\?create\@IntPair\@\@SAPEAU1\@XZ}} : $@convention(c) () -> IntPair

// CHECK-LABEL: sil{{ \[available .*\] | }}[clang IntPair.test] @{{_ZNK7IntPair4testEv|\?test\@IntPair\@\@QEBAHXZ}} : $@convention(cxx_method) (@in_guaranteed IntPair) -> Int32
