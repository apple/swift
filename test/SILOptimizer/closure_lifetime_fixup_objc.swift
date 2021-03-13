// RUN: %target-swift-frontend %s -sil-verify-all -emit-sil -disable-copy-propagation -o - -I %S/Inputs/usr/include | %FileCheck %s
// REQUIRES: objc_interop

// Using -disable-copy-propagation to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

import Foundation
import ClosureLifetimeFixupObjC

@objc
public protocol DangerousEscaper {
  @objc
  func malicious(_ mayActuallyEscape: () -> ())
}

// CHECK: sil @$s27closure_lifetime_fixup_objc19couldActuallyEscapeyyyyc_AA16DangerousEscaper_ptF : $@convention(thin) (@guaranteed @callee_guaranteed () -> (), @guaranteed DangerousEscaper) -> () {
// CHECK: bb0([[ARG:%.*]] : $@callee_guaranteed () -> (), [[SELF:%.*]] : $DangerousEscaper):
// CHECK:   [[OE:%.*]] = open_existential_ref [[SELF]]

// Extend the lifetime to the end of this function (2).
// CHECK:   strong_retain [[ARG]] : $@callee_guaranteed () -> ()

// CHECK:   [[NE:%.*]] = convert_escape_to_noescape [[ARG]] : $@callee_guaranteed () -> () to $@noescape @callee_guaranteed () -> ()
// CHECK:   [[WITHOUT_ACTUALLY_ESCAPING_THUNK:%.*]] = function_ref @$sIg_Ieg_TR : $@convention(thin) (@noescape @callee_guaranteed () -> ()) -> ()
// CHECK:   [[C:%.*]] = partial_apply [callee_guaranteed] [[WITHOUT_ACTUALLY_ESCAPING_THUNK]]([[NE]]) : $@convention(thin) (@noescape @callee_guaranteed () -> ()) -> ()

// Sentinel without actually escaping closure (3).
// CHECK:   [[SENTINEL:%.*]] = mark_dependence [[C]] : $@callee_guaranteed () -> () on [[NE]] : $@noescape @callee_guaranteed () -> ()

// Copy of sentinel (4).
// CHECK:   strong_retain [[SENTINEL]] : $@callee_guaranteed () -> ()
// CHECK:   [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage @callee_guaranteed () -> ()
// CHECK:   [[CLOSURE_ADDR:%.*]] = project_block_storage [[BLOCK_STORAGE]] : $*@block_storage @callee_guaranteed () -> ()
// CHECK:   store [[SENTINEL]] to [[CLOSURE_ADDR]] : $*@callee_guaranteed () -> ()
// CHECK:   [[BLOCK_INVOKE:%.*]] = function_ref @$sIeg_IyB_TR : $@convention(c) (@inout_aliasable @block_storage @callee_guaranteed () -> ()) -> ()
// CHECK:   [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] : $*@block_storage @callee_guaranteed () -> (), invoke [[BLOCK_INVOKE]] : $@convention(c) (@inout_aliasable @block_storage @callee_guaranteed () -> ()) -> (), type $@convention(block) @noescape () -> ()

// Copy of sentinel closure (5).
// CHECK:   [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]] : $@convention(block) @noescape () -> ()

// Release of sentinel closure (3).
// CHECK:   destroy_addr [[CLOSURE_ADDR]] : $*@callee_guaranteed () -> ()
// CHECK:   dealloc_stack [[BLOCK_STORAGE]] : $*@block_storage @callee_guaranteed () -> ()

// CHECK:   [[METH:%.*]] = objc_method [[OE]] : $@opened("{{.*}}") DangerousEscaper, #DangerousEscaper.malicious!foreign : <Self where Self : DangerousEscaper> (Self) -> (() -> ()) -> (), $@convention(objc_method) <τ_0_0 where τ_0_0 : DangerousEscaper> (@convention(block) @noescape () -> (), τ_0_0) -> ()
// CHECK:   apply [[METH]]<@opened("{{.*}}") DangerousEscaper>([[BLOCK_COPY]], [[OE]]) : $@convention(objc_method) <τ_0_0 where τ_0_0 : DangerousEscaper> (@convention(block) @noescape () -> (), τ_0_0) -> ()

// Release sentinel closure copy (5).
// CHECK:   strong_release [[BLOCK_COPY]] : $@convention(block) @noescape () -> ()
// CHECK:   [[ESCAPED:%.*]] = is_escaping_closure [objc] [[SENTINEL]]
// CHECK:   cond_fail [[ESCAPED]] : $Builtin.Int1

// Release of sentinel copy (4).
// CHECK:   strong_release [[SENTINEL]]

// Extendend lifetime (2).
// CHECK:   strong_release [[ARG]]
// CHECK:   return
// CHECK: } // end sil function '$s27closure_lifetime_fixup_objc19couldActuallyEscapeyyyyc_AA16DangerousEscaper_ptF'
public func couldActuallyEscape(_ closure: @escaping () -> (), _ villian: DangerousEscaper) {
  villian.malicious(closure)
}

// Make sure that we respect the ownership verifier.
//
// CHECK-LABEL: sil @$s27closure_lifetime_fixup_objc27couldActuallyEscapeWithLoopyyyyc_AA16DangerousEscaper_ptF : $@convention(thin) (@guaranteed @callee_guaranteed () -> (), @guaranteed DangerousEscaper) -> () {
public func couldActuallyEscapeWithLoop(_ closure: @escaping () -> (), _ villian: DangerousEscaper) {
  for _ in 0..<2 {
    villian.malicious(closure)
  }
}

// We need to store nil on the back-edge.
// CHECK-LABEL: sil @$s27closure_lifetime_fixup_objc9dontCrashyyF : $@convention(thin) () -> () {
// CHECK: bb0:
// CHECK:   [[NONE:%.*]] = enum $Optional<{{.*}}>, #Optional.none!enumelt
// CHECK:   br bb1
//
// CHECK: bb1:
// CHECK:   br bb2
//
// CHECK: bb2:
// CHECK:   br [[LOOP_HEADER_BB:bb[0-9]+]]([[NONE]]
//
// CHECK: [[LOOP_HEADER_BB]]([[LOOP_IND_VAR:%.*]] : $Optional
// CHECK:   switch_enum {{.*}} : $Optional<Int>, case #Optional.some!enumelt: [[SUCC_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
//
// CHECK: [[SUCC_BB]](
// CHECK:   [[THUNK_FUNC:%.*]] = function_ref @$sIg_Ieg_TR :
// CHECK:   [[PAI:%.*]] = partial_apply [callee_guaranteed] [[THUNK_FUNC]](
// CHECK:   [[MDI:%.*]] = mark_dependence [[PAI]]
// CHECK:   strong_retain [[MDI]]
// CHECK:   [[BLOCK_SLOT:%.*]] = alloc_stack $@block_storage @callee_guaranteed () -> ()
// CHECK:   [[BLOCK_PROJ:%.*]] = project_block_storage [[BLOCK_SLOT]]
// CHECK:   store [[MDI]] to [[BLOCK_PROJ]] :
// CHECK:   [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_SLOT]]
// CHECK:   release_value [[LOOP_IND_VAR]]
// CHECK:   [[SOME:%.*]] = enum $Optional<{{.*}}>, #Optional.some!enumelt, [[MDI]]
// CHECK:   [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]]
// CHECK:   destroy_addr [[BLOCK_PROJ]]
// CHECK:   [[DISPATCH_SYNC_FUNC:%.*]] = function_ref @dispatch_sync :
// CHECK:   apply [[DISPATCH_SYNC_FUNC]]({{%.*}}, [[BLOCK_COPY]])
// CHECK:   strong_release [[BLOCK_COPY]]
// CHECK:   is_escaping_closure [objc] [[SOME]]
// CHECK:   release_value [[SOME]]
// CHECK:   [[NONE_FOR_BACKEDGE:%.*]] = enum $Optional<{{.*}}>, #Optional.none
// CHECK:   br [[LOOP_HEADER_BB]]([[NONE_FOR_BACKEDGE]] :
//
// CHECK: [[NONE_BB]]:
// CHECK:   release_value [[LOOP_IND_VAR]]
// CHECK:   return
// CHECK: } // end sil function '$s27closure_lifetime_fixup_objc9dontCrashyyF'
public func dontCrash() {
  for i in 0 ..< 2 {
    let queue = DispatchQueue(label: "Foo")
    queue.sync { }
  }
}

@_silgen_name("getDispatchQueue")
func getDispatchQueue() -> DispatchQueue

// We must not release the closure after calling super.deinit.
// CHECK: sil hidden @$s27closure_lifetime_fixup_objc1CCfD : $@convention(method) (@owned C) -> () {
// CHECK: bb0([[SELF:%.*]] : $C):
// CHECK:   [[F:%.*]] = function_ref @$s27closure_lifetime_fixup_objc1CCfdyyXEfU_
// CHECK:   [[PA:%.*]] = partial_apply [callee_guaranteed] [[F]]([[SELF]])
// CHECK:   [[DEINIT:%.*]] = objc_super_method [[SELF]] : $C, #NSObject.deinit!deallocator.foreign
// CHECK:   strong_release [[PA]]
// CHECK:   [[SUPER:%.*]] = upcast [[SELF]] : $C to $NSObject
// CHECK-NEXT:   apply [[DEINIT]]([[SUPER]]) : $@convention(objc_method) (NSObject) -> ()
// CHECK-NEXT:   [[T:%.*]] = tuple ()
// CHECK-NEXT:   return [[T]] : $()
// CHECK: } // end sil function '$s27closure_lifetime_fixup_objc1CCfD'
class C: NSObject {
  deinit {
    getDispatchQueue().sync(execute: { _ = self })
  }
}

// Make sure even if we have a loop, we do not release the value after calling super.deinit
// CHECK-LABEL: sil hidden @$s27closure_lifetime_fixup_objc9CWithLoopCfD : $@convention(method) (@owned CWithLoop) -> () {
// CHECK:        [[METH:%.*]] = objc_super_method
// CHECK-NEXT:   release_value
// CHECK-NEXT:   release_value
// CHECK-NEXT:   upcast {{%.*}} : $CWithLoop to $NSObject
// CHECK-NEXT:   apply [[METH]]({{%.*}}) : $@convention(objc_method) (NSObject) -> ()
// CHECK-NEXT:   tuple ()
// CHECK-NEXT:   return
// CHECK: } // end sil function '$s27closure_lifetime_fixup_objc9CWithLoopCfD'
class CWithLoop : NSObject {
  deinit {
    for _ in 0..<2 {
      getDispatchQueue().sync(execute: { _ = self })
    }
  }
}

class CWithLoopReturn : NSObject {
  deinit {
    for _ in 0..<2 {
      getDispatchQueue().sync(execute: { _ = self })
      return
    }
  }
}

// Make sure that we obey ownership invariants when we emit load_borrow.
internal typealias MyBlock = @convention(block) () -> Void
func getBlock(noEscapeBlock: () -> Void ) -> MyBlock {
  return block_create_noescape(noEscapeBlock)
}

func getBlockWithLoop(noEscapeBlock: () -> Void ) -> MyBlock {
  for _ in 0..<2 {
    return block_create_noescape(noEscapeBlock)
  }
  return (Optional<MyBlock>.none)!
}

