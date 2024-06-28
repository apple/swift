// RUN: %target-swift-emit-silgen -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -disable-availability-checking | %FileCheck %s

// REQUIRES: objc_interop
// REQUIRES: concurrency

import gizmo

// Although we don't ever expose initializers and methods of generic classes
// to ObjC yet, a generic subclass of an ObjC class must still use ObjC
// deallocation.

// CHECK-NOT: sil hidden [ossa] @$sSo7GenericCfd
// CHECK-NOT: sil hidden [ossa] @$sSo8NSObjectCfd

class Generic<T>: NSObject, ObjCProtocol {
  var x: Int = 10

  // CHECK-LABEL: sil private [thunk] [ossa] @$s18objc_generic_class7GenericC5evokeyyFTo : $@convention(objc_method) <T> (Generic<T>) -> () {
  // CHECK:         [[FN:%.*]] = function_ref @$s18objc_generic_class7GenericC5evokeyyF
  // CHECK-NEXT:    apply [[FN]]<T>
  func evoke() {}

  // CHECK-LABEL: sil private [thunk] [ossa] @$s18objc_generic_class7GenericC10evokeAsyncyyYaFTo : $@convention(objc_method) <T> (@convention(block) () -> (), Generic<T>) -> () {
  // CHECK:         [[FN:%.*]] = function_ref @$s18objc_generic_class7GenericC10evokeAsyncyyYaFyyYacfU_To
  // CHECK-NEXT:     partial_apply [callee_guaranteed] [[FN]]<T>

  // CHECK-LABEL: sil shared [thunk] [ossa] @$s18objc_generic_class7GenericC10evokeAsyncyyYaFyyYacfU_To : $@convention(thin) @Sendable @async <T> (@convention(block) () -> (), Generic<T>) -> ()
  // CHECK:         [[FN:%.*]] = function_ref @$s18objc_generic_class7GenericC10evokeAsyncyyYaF : $@convention(method) @async <τ_0_0> (@guaranteed Generic<τ_0_0>) -> ()
  // CHECK-NEXT:    apply [[FN]]<T>
  func evokeAsync() async {}

  // CHECK-LABEL: sil hidden [ossa] @$s18objc_generic_class7GenericCfD : $@convention(method) <T> (@owned Generic<T>) -> () {
  // CHECK:       bb0({{%.*}} : @owned $Generic<T>):
  // CHECK: } // end sil function '$s18objc_generic_class7GenericCfD'
  // CHECK-LABEL: sil private [thunk] [ossa] @$s18objc_generic_class7GenericCfDTo : $@convention(objc_method) <T> (Generic<T>) -> () {
  // CHECK:       bb0([[SELF:%.*]] : @unowned $Generic<T>):
  // CHECK:         [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:         [[NATIVE:%.*]] = function_ref @$s18objc_generic_class7GenericCfD
  // CHECK:         apply [[NATIVE]]<T>([[SELF_COPY]])
  // CHECK:       } // end sil function '$s18objc_generic_class7GenericCfDTo'
  deinit {
    // Don't blow up when 'self' is referenced inside an @objc deinit method
    // of a generic class. <rdar://problem/16325525>
    self.x = 0
  }
}

// CHECK-NOT: sil hidden [ossa] @$s18objc_generic_class7GenericCfd
// CHECK-NOT: sil hidden [ossa] @$sSo8NSObjectCfd

@objc protocol ObjCProtocol {
  func evoke()
  func evokeAsync() async
}

// CHECK-LABEL: sil hidden [ossa] @$s18objc_generic_class11SubGeneric1CfD : $@convention(method) <U, V> (@owned SubGeneric1<U, V>) -> () {
// CHECK:       bb0([[SELF:%.*]] : @owned $SubGeneric1<U, V>):
// CHECK:         [[SUPER_DEALLOC:%.*]] = objc_super_method [[SELF]] : $SubGeneric1<U, V>, #Generic.deinit!deallocator.foreign : <T> (Generic<T>) -> () -> (), $@convention(objc_method) <τ_0_0> (Generic<τ_0_0>) -> ()
// CHECK:         [[SUPER:%.*]] = upcast [[SELF:%.*]] : $SubGeneric1<U, V> to $Generic<Int>
// CHECK:         apply [[SUPER_DEALLOC]]<Int>([[SUPER]])
class SubGeneric1<U, V>: Generic<Int> {
}


// Ensure that the verifier doesn't reject @objc functions where all of the
// generic parameters have been same-typed to concrete types.
public struct GenericStruct<T> { }

public extension GenericStruct where T == String {
  public class Y {
    @objc public func f() -> String { "hello" }
  }
}

// rdar://129187133 - handle generic @objc thunks properly
actor GenericActor<T> : SendableObjCProtocol {
  // CHECK-LABEL: sil private [thunk] [ossa] @$s18objc_generic_class12GenericActorC10evokeAsyncyyYaFTo : $@convention(objc_method) <T> (@convention(block) () -> (), @sil_isolated GenericActor<T>) -> ()
  // CHECK:          [[FN:%.*]] = function_ref @$s18objc_generic_class12GenericActorC10evokeAsyncyyYaFyyYacfU_To : $@convention(thin) @Sendable @async <τ_0_0> (@convention(block) () -> (), @sil_isolated GenericActor<τ_0_0>) -> ()
  // CHECK-NEXT:     partial_apply [callee_guaranteed] [[FN]]<T>
  func evokeAsync() async {}

  // CHECK-LABEL: sil shared [thunk] [ossa] @$s18objc_generic_class12GenericActorC10evokeAsyncyyYaFyyYacfU_To : $@convention(thin) @Sendable @async <T> (@convention(block) () -> (), @sil_isolated GenericActor<T>) -> ()
  // CHECK:         [[FN:%.*]] = function_ref @$s18objc_generic_class12GenericActorC10evokeAsyncyyYaF : $@convention(method) @async <τ_0_0> (@sil_isolated @guaranteed GenericActor<τ_0_0>) -> ()
  // CHECK-NEXT:     apply [[FN]]<T>
}

@objc protocol SendableObjCProtocol : Sendable {
  func evokeAsync() async
}
