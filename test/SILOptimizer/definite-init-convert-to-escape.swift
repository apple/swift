// RUN: %target-swift-frontend -module-name A -verify -emit-sil -import-objc-header %S/Inputs/Closure.h -disable-copy-propagation -disable-objc-attr-requires-foundation-module %s | %FileCheck %s
// RUN: %target-swift-frontend -module-name A -verify -emit-sil -import-objc-header %S/Inputs/Closure.h -disable-copy-propagation -disable-objc-attr-requires-foundation-module -Xllvm -sil-disable-convert-escape-to-noescape-switch-peephole %s | %FileCheck %s --check-prefix=NOPEEPHOLE
//
// Using -disable-copy-propagation to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

// REQUIRES: objc_interop

import Foundation

// Make sure that we keep the escaping closures alive accross the ultimate call.
// CHECK-LABEL: sil @$s1A19bridgeNoescapeBlock5optFn0D3Fn2yySSSgcSg_AFtF
// CHECK: bb0
// CHECK:  retain_value %0
// CHECK:  retain_value %0
// CHECK: bb1
// CHECK:  convert_escape_to_noescape %
// CHECK:  strong_release
// CHECK: bb5
// CHECK:  retain_value %1
// CHECK:  retain_value %1
// CHECK: bb6
// CHECK:  convert_escape_to_noescape %
// CHECK:  strong_release
// CHECK: bb10
// CHECK:  [[F:%.*]] = function_ref @noescapeBlock3
// CHECK:  apply [[F]]
// CHECK:  release_value {{.*}} : $Optional<NSString>
// CHECK:  release_value %1 : $Optional<@callee_guaranteed (@guaranteed Optional<String>) -> ()>
// CHECK:  release_value {{.*}} : $Optional<@convention(block) @noescape (Optional<NSString>) -> ()>
// CHECK:  release_value %0 : $Optional<@callee_guaranteed (@guaranteed Optional<String>) -> ()>
// CHECK:  release_value {{.*}} : $Optional<@convention(block) @noescape (Optional<NSString>)
public func bridgeNoescapeBlock( optFn: ((String?) -> ())?, optFn2: ((String?) -> ())?) {
  noescapeBlock3(optFn, optFn2, "Foobar")
}


@_silgen_name("_returnOptionalEscape")
public func returnOptionalEscape() -> (() ->())?

// Make sure that we keep the escaping closure alive accross the ultimate call.

// CHECK-LABEL: sil @$s1A19bridgeNoescapeBlockyyF : $@convention(thin) () -> () {
// CHECK: bb0:
// CHECK:  [[NONE:%.*]] = enum $Optional<{{.*}}>, #Optional.none!enumelt
// CHECK:  [[V0:%.*]] = function_ref @_returnOptionalEscape
// CHECK:  [[V1:%.*]] = apply [[V0]]
// CHECK:  retain_value [[V1]]
// CHECK:  switch_enum [[V1]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
//
// CHECK: [[SOME_BB]]([[V2:%.*]] : $@callee_guaranteed () -> ()):
// CHECK:  [[V1_UNWRAPPED:%.*]] = unchecked_enum_data [[V1]]
// CHECK:  [[CVT:%.*]] = convert_escape_to_noescape [[V1_UNWRAPPED]]
// CHECK:  [[SOME:%.*]] = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[CVT]]
// CHECK:  strong_release [[V2]]
// CHECK:  br [[NEXT_BB:bb[0-9]+]]([[SOME]] :
//
// CHECK: [[NEXT_BB]]([[SOME_PHI:%.*]] :
// CHECK:   switch_enum [[SOME_PHI]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB_2:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB_2:bb[0-9]+]]
//
// CHECK: [[SOME_BB_2]]([[SOME_PHI_PAYLOAD:%.*]] :
// CHECK:   [[PAI:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[SOME_PHI_PAYLOAD]])
// CHECK:   [[MDI:%.*]] = mark_dependence [[PAI]]
// CHECK:   strong_retain [[MDI]]
// CHECK:   [[BLOCK_SLOT:%.*]] = alloc_stack
// CHECK:   [[BLOCK_PROJ:%.*]] = project_block_storage [[BLOCK_SLOT]]
// CHECK:   store [[MDI]] to [[BLOCK_PROJ]]
// CHECK:   [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_SLOT]]
// CHECK:   release_value [[NONE]]
// CHECK:   [[SOME_2:%.*]] = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[MDI]]
// CHECK:   [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]]
// CHECK:   [[BLOCK_SOME:%.*]]  = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[BLOCK_COPY]]
// CHECK:   br bb5([[BLOCK_SOME]] : ${{.*}}, [[SOME_2]] :
//
// CHECK: bb4:
// CHECK:   [[NONE_BLOCK:%.*]] = enum $Optional<{{.*}}>, #Optional.none!enumelt
// CHECK:   br bb5([[NONE_BLOCK]] : {{.*}}, [[NONE]] :
//
// CHECK: bb5([[BLOCK_PHI:%.*]] : $Optional<{{.*}}>, [[SWIFT_CLOSURE_PHI:%.*]] :
// CHECK:  [[F:%.*]] = function_ref @noescapeBlock
// CHECK:  apply [[F]]([[BLOCK_PHI]])
// CHECK:  release_value [[BLOCK_PHI]]
// CHECK:  release_value [[SWIFT_CLOSURE_PHI]]
// CHECK-NEXT: return

// NOPEEPHOLE-LABEL: sil @$s1A19bridgeNoescapeBlockyyF : $@convention(thin) () -> () {
// NOPEEPHOLE: bb0:
// NOPEEPHOLE:  [[NONE_1:%.*]] = enum $Optional<{{.*}}>, #Optional.none!enumelt
// NOPEEPHOLE:  [[NONE_2:%.*]] = enum $Optional<{{.*}}>, #Optional.none!enumelt
// NOPEEPHOLE:  [[V0:%.*]] = function_ref @_returnOptionalEscape
// NOPEEPHOLE:  [[V1:%.*]] = apply [[V0]]
// NOPEEPHOLE:  switch_enum [[V1]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
//
// NOPEEPHOLE: [[SOME_BB]]([[V2:%.*]]: $@callee_guaranteed () -> ()):
// NOPEEPHOLE-NEXT:  release_value [[NONE_2]]
// NOPEEPHOLE-NEXT:  [[CVT:%.*]] = convert_escape_to_noescape [[V2]]
// NOPEEPHOLE-NEXT:  [[SOME:%.*]] = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[V2]]
// NOPEEPHOLE-NEXT:  [[NOESCAPE_SOME:%.*]] = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[CVT]]
// NOPEEPHOLE-NEXT:  br bb2([[NOESCAPE_SOME]] : $Optional<{{.*}}>, [[SOME]] :
//
// NOPEEPHOLE: bb2([[NOESCAPE_SOME:%.*]] : $Optional<{{.*}}>, [[SOME:%.*]] :
// NOPEEPHOLE:   switch_enum [[NOESCAPE_SOME]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB_2:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB_2:bb[0-9]+]]
//
// NOPEEPHOLE: [[SOME_BB_2]](
// NOPEEPHOLE:   br bb5
//
// NOPEEPHOLE: [[NONE_BB_2]]:
// NOPEEPHOLE:   br bb5
//
// NOPEEPHOLE: bb5([[BLOCK_PHI:%.*]] : $Optional<{{.*}}>, [[SWIFT_CLOSURE_PHI:%.*]] :
// NOPEEPHOLE-NEXT: function_ref noescapeBlock
// NOPEEPHOLE-NEXT:  [[F:%.*]] = function_ref @noescapeBlock :
// NOPEEPHOLE-NEXT:  apply [[F]]([[BLOCK_PHI]])
// NOPEEPHOLE-NEXT:  release_value [[BLOCK_PHI]]
// NOPEEPHOLE-NEXT:  tuple
// NOPEEPHOLE-NEXT:  release_value [[SOME]]
// NOPEEPHOLE-NEXT:  release_value [[SWIFT_CLOSURE_PHI]]
// NOPEEPHOLE-NEXT: return
// NOPEEPHOLE: } // end sil function '$s1A19bridgeNoescapeBlockyyF'
public func bridgeNoescapeBlock() {
  noescapeBlock(returnOptionalEscape())
}


