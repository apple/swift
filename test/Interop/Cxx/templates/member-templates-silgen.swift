// RUN: %target-swift-emit-sil %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s

import MemberTemplates

// CHECK-LABEL: sil hidden @$s4main9basicTestyyF : $@convention(thin) () -> ()

// CHECK: [[ADD:%.*]] = function_ref @{{_ZN18HasMemberTemplates17addSameTypeParamsIiEET_S1_S1_|\?\?\$addSameTypeParams@H@HasMemberTemplates@@QEAAHHH@Z}} : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32
// CHECK: apply [[ADD]]({{.*}}) : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK: [[ADD_TWO_TEMPLATES:%.*]] = function_ref @{{_ZN18HasMemberTemplates18addMixedTypeParamsIiiEET_S1_T0_|\?\?\$addMixedTypeParams@HH@HasMemberTemplates@@QEAAHHH@Z}} : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32
// CHECK: apply [[ADD_TWO_TEMPLATES]]({{.*}}) : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK: [[ADD_ALL:%.*]] = function_ref @{{_ZN18HasMemberTemplates6addAllIiiEEiiT_T0_|\?\?\$addAll@HH@HasMemberTemplates@@QEAAHHHH@Z}} : $@convention(cxx_method) (Int32, Int32, Int32, @inout HasMemberTemplates) -> Int32
// CHECK: apply [[ADD_ALL]]({{.*}}) : $@convention(cxx_method) (Int32, Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK: [[DO_NOTHING:%.*]] = function_ref @{{_ZN18HasMemberTemplates17doNothingConstRefIiEEvRKT_|\?\?\$doNothingConstRef@H@HasMemberTemplates@@QEAAXAEBH@Z}} : $@convention(cxx_method) (@in_guaranteed Int32, @inout HasMemberTemplates) -> ()
// CHECK: apply [[DO_NOTHING]]({{.*}}) : $@convention(cxx_method) (@in_guaranteed Int32, @inout HasMemberTemplates) -> ()

// CHECK-LABEL: end sil function '$s4main9basicTestyyF'
func basicTest() {
  var i: Int32 = 0
  var obj = HasMemberTemplates()
  obj.addSameTypeParams(i, i)
  obj.addMixedTypeParams(i, i)
  obj.addAll(i, i, i)
  obj.doNothingConstRef(i)
}

// CHECK-LABEL: sil [clang HasMemberTemplates.addSameTypeParams] @{{_ZN18HasMemberTemplates17addSameTypeParamsIiEET_S1_S1_|\?\?\$addSameTypeParams@H@HasMemberTemplates@@QEAAHHH@Z}} : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK-LABEL: sil [clang HasMemberTemplates.addMixedTypeParams] @{{_ZN18HasMemberTemplates18addMixedTypeParamsIiiEET_S1_T0_|\?\?\$addMixedTypeParams@HH@HasMemberTemplates@@QEAAHHH@Z}} : $@convention(cxx_method) (Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK-LABEL: sil [clang HasMemberTemplates.addAll] @{{_ZN18HasMemberTemplates6addAllIiiEEiiT_T0_|\?\?\$addAll@HH@HasMemberTemplates@@QEAAHHHH@Z}} : $@convention(cxx_method) (Int32, Int32, Int32, @inout HasMemberTemplates) -> Int32

// CHECK-LABEL: sil [clang HasMemberTemplates.doNothingConstRef] @{{_ZN18HasMemberTemplates17doNothingConstRefIiEEvRKT_|\?\?\$doNothingConstRef@H@HasMemberTemplates@@QEAAXAEBH@Z}} : $@convention(cxx_method) (@in_guaranteed Int32, @inout HasMemberTemplates) -> ()

// CHECK-LABEL: sil hidden @$s4main12testSetValueyyF : $@convention(thin) () -> ()

// CHECK: [[SET_VALUE:%.*]] = function_ref @{{_ZN32TemplateClassWithMemberTemplatesIiE8setValueIlEEvT_|\?\?\$setValue@_J@\?\$TemplateClassWithMemberTemplates@H@@QEAAX_J@Z}} : $@convention(cxx_method) (Int, @inout TemplateClassWithMemberTemplates<Int32>) -> ()
// CHECK: apply [[SET_VALUE]]({{.*}}) : $@convention(cxx_method) (Int, @inout TemplateClassWithMemberTemplates<Int32>) -> ()

// CHECK-LABEL: end sil function '$s4main12testSetValueyyF'
func testSetValue() {
  var w = IntWrapper(11)
  w.setValue(42)
}

// CHECK-LABEL: sil hidden @$s4main17testStaticMembersyyF : $@convention(thin) () -> ()

// CHECK: [[ADD_FN:%.*]] = function_ref @{{_ZN24HasStaticMemberTemplates3addIlEET_S1_S1_|\?\?\$add@_J@HasStaticMemberTemplates@@SA_J_J0@Z}} : $@convention(c) (Int, Int) -> Int
// CHECK: apply [[ADD_FN]]({{.*}}) : $@convention(c) (Int, Int) -> Int

// CHECK: [[ADD_TWO_TEMPLATES_FN:%.*]] = function_ref @{{_ZN24HasStaticMemberTemplates15addTwoTemplatesIlcEET_S1_T0_|\?\?\$addTwoTemplates@_JD@HasStaticMemberTemplates@@SA_J_JD@Z}} : $@convention(c) (Int, Int8) -> Int
// CHECK: apply [[ADD_TWO_TEMPLATES_FN]]({{.*}}) : $@convention(c) (Int, Int8) -> Int

// CHECK: [[REMOVE_REFERENCE_FN:%.*]] = function_ref @{{_ZN24HasStaticMemberTemplates15removeReferenceIlEET_RS1_|\?\?\$removeReference@_J@HasStaticMemberTemplates@@SA_JAEA_J@Z}} : $@convention(c) (@inout Int) -> Int
// CHECK: apply [[REMOVE_REFERENCE_FN]]({{.*}}) : $@convention(c) (@inout Int) -> Int

// CHECK-LABEL: end sil function '$s4main17testStaticMembersyyF'
func testStaticMembers() {
  var x: Int = 0
  let y: CChar = 0
  HasStaticMemberTemplates.add(x, x)
  HasStaticMemberTemplates.addTwoTemplates(x, y)
  HasStaticMemberTemplates.removeReference(&x)
}

// CHECK: sil hidden_external [clang HasStaticMemberTemplates.add] @{{_ZN24HasStaticMemberTemplates3addIlEET_S1_S1_|\?\?\$add@_J@HasStaticMemberTemplates@@SA_J_J0@Z}} : $@convention(c) (Int, Int) -> Int

// CHECK: sil hidden_external [clang HasStaticMemberTemplates.addTwoTemplates] @{{_ZN24HasStaticMemberTemplates15addTwoTemplatesIlcEET_S1_T0_|\?\?\$addTwoTemplates@_JD@HasStaticMemberTemplates@@SA_J_JD@Z}} : $@convention(c) (Int, Int8) -> Int

// CHECK: sil hidden_external [clang HasStaticMemberTemplates.removeReference] @{{_ZN24HasStaticMemberTemplates15removeReferenceIlEET_RS1_|\?\?\$removeReference@_J@HasStaticMemberTemplates@@SA_JAEA_J@Z}} : $@convention(c) (@inout Int) -> Int
