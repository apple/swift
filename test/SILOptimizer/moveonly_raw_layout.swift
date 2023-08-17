// RUN: %target-swift-frontend -enable-experimental-feature BuiltinModule -enable-experimental-feature RawLayout -emit-sil %s | %FileCheck %s

import Builtin

@_silgen_name("init_lock")
func init_lock(_: Builtin.RawPointer)

@_silgen_name("deinit_lock")
func deinit_lock(_: Builtin.RawPointer)

@_rawLayout(size: 4, alignment: 4)
struct Lock: ~Copyable {
    var _address: Builtin.RawPointer { return Builtin.addressOfBorrow(self) }

    // CHECK-LABEL: // Lock.init()
    // CHECK-NEXT: sil{{.*}} @[[INIT:\$.*4LockV.*fC]] :
    init() {
        // CHECK-NOT: destroy_addr
        // CHECK: builtin "zeroInitializer"<Lock>
        // CHECK-NOT: destroy_addr
        // CHECK: [[F:%.*]] = function_ref @init_lock
        // CHECK: apply [[F]](
        // CHECK-NOT: destroy_addr
        // CHECK: } // end sil function '[[INIT]]'
        init_lock(_address)
    }

    // CHECK-LABEL: // Lock.deinit
    // CHECK-NEXT: sil{{.*}} @[[DEINIT:\$.*4LockV.*fD]] :
    deinit {
        // CHECK-NOT: destroy_addr
        // CHECK: [[F:%.*]] = function_ref @deinit_lock
        // CHECK: apply [[F]](
        // CHECK-NOT: destroy_addr
        // CHECK: } // end sil function '[[DEINIT]]'
        deinit_lock(_address)
    }
}

@_silgen_name("borrow_lock")
func borrow_lock(_: borrowing Lock)

// CHECK-LABEL: sil{{.*}} @{{.*}}7useLock
public func useLock() {
    // CHECK: [[L:%.*]] = alloc_stack
    // CHECK-NOT: destroy_addr [[L]] :
    // CHECK: [[F:%.*]] = function_ref @[[INIT]]
    // CHECK: apply [[F]]([[L]], 
    var l = Lock()
    // CHECK-NOT: destroy_addr [[L]] :
    // CHECK: [[L_BORROW:%.*]] = begin_access [read] [static] [[L]] :
    // CHECK: [[F:%.*]] = function_ref @borrow_lock
    // CHECK: apply [[F]]([[L_BORROW]])
    // CHECK: end_access [[L_BORROW]]
    borrow_lock(l)

    // CHECK: [[L2:%.*]] = alloc_stack
    // CHECK-NOT: destroy_addr [[L2]] :
    // CHECK: [[F:%.*]] = function_ref @[[INIT]]
    // CHECK: apply [[F]]([[L2]], 
    // CHECK: [[L_INOUT:%.*]] = begin_access [modify] [static] [[L]] :
    // CHECK: destroy_addr [[L]] :
    // CHECK: copy_addr [take] [[L2]] to [init] [[L_INOUT]]
    // CHECK: end_access [[L_INOUT]]
    // CHECK-NOT: destroy_addr [[L2]] :
    // CHECK: dealloc_stack [[L2]]
    l = Lock()
    // CHECK-NOT: destroy_addr [[L]] :
    // CHECK: [[L_BORROW:%.*]] = begin_access [read] [static] [[L]] :
    // CHECK: [[F:%.*]] = function_ref @borrow_lock
    // CHECK: apply [[F]]([[L_BORROW]])
    // CHECK: end_access [[L_BORROW]]
    borrow_lock(l)

    // CHECK: destroy_addr [[L]]
    // CHECK: dealloc_stack [[L]]
}
