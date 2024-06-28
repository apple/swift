// RUN: %target-swift-frontend -O -emit-sil -disable-availability-checking %s | %IRGenFileCheck %s

// REQUIRES: synchronization

import Synchronization

@_silgen_name("testInt")
func testInt(_: Int)

//===----------------------------------------------------------------------===//
// Ensure that we don't destroy the atomic before operations
//===----------------------------------------------------------------------===//

// CHECK-LABEL: sil {{.*}} @localLoad {{.*}} {
// CHECK:         [[ATOMIC:%.*]] = alloc_stack [lexical] [var_decl] $Atomic<Int>
// CHECK:         [[ATOMIC_PTR:%.*]] = address_to_pointer [[ATOMIC]]
// CHECK:         builtin "atomicload_monotonic_Int[[PTR_SIZE]]"([[ATOMIC_PTR]] : $Builtin.RawPointer)
// CHECK:         destroy_addr [[ATOMIC]] : $*Atomic<Int>
// CHECK-NEXT:    dealloc_stack [[ATOMIC]] : $*Atomic<Int>
// CHECK-LABEL: } // end sil function 'localLoad'
@_silgen_name("localLoad")
func localLoad() -> Int {
  let x = Atomic(128)
  return x.load(ordering: .relaxed)
}

// CHECK-LABEL: sil {{.*}} @localStore {{.*}} {
// CHECK:         [[ATOMIC:%.*]] = alloc_stack [lexical] [var_decl] $Atomic<Int>
// CHECK:         [[ATOMIC_PTR:%.*]] = address_to_pointer [[ATOMIC]]
// CHECK:         builtin "atomicstore_release_Int[[PTR_SIZE]]"([[ATOMIC_PTR]] : $Builtin.RawPointer
// CHECK:         destroy_addr [[ATOMIC]] : $*Atomic<Int>
// CHECK-NEXT:    dealloc_stack [[ATOMIC]] : $*Atomic<Int>
// CHECK-LABEL: } // end sil function 'localStore'
@_silgen_name("localStore")
func localStore() {
  let x = Atomic(128)
  x.store(0, ordering: .releasing)
}

// CHECK-LABEL: sil {{.*}} @localExchange {{.*}} {
// CHECK:         [[ATOMIC:%.*]] = alloc_stack [lexical] [var_decl] $Atomic<Int>
// CHECK:         [[ATOMIC_PTR:%.*]] = address_to_pointer [[ATOMIC]]
// CHECK:         builtin "atomicrmw_xchg_acquire_Int[[PTR_SIZE]]"([[ATOMIC_PTR]] : $Builtin.RawPointer
// CHECK:         destroy_addr [[ATOMIC]] : $*Atomic<Int>
// CHECK-NEXT:    dealloc_stack [[ATOMIC]] : $*Atomic<Int>
// CHECK-LABEL: } // end sil function 'localExchange'
@_silgen_name("localExchange")
func localExchange() -> Int {
  let x = Atomic(128)
  return x.exchange(0, ordering: .acquiring)
}

// CHECK-LABEL: sil {{.*}} @localCompareExchange {{.*}} {
// CHECK:         [[ATOMIC:%.*]] = alloc_stack [lexical] [var_decl] $Atomic<Int>
// CHECK:         [[ATOMIC_PTR:%.*]] = address_to_pointer [[ATOMIC]]
// CHECK:         builtin "cmpxchg_seqcst_seqcst_Int[[PTR_SIZE]]"([[ATOMIC_PTR]] : $Builtin.RawPointer
// CHECK:         destroy_addr [[ATOMIC]] : $*Atomic<Int>
// CHECK-NEXT:    dealloc_stack [[ATOMIC]] : $*Atomic<Int>
// CHECK-LABEL: } // end sil function 'localCompareExchange'
@_silgen_name("localCompareExchange")
func localCompareExchange() -> (exchanged: Bool, original: Int) {
  let x = Atomic(128)
  return x.compareExchange(
    expected: 128,
    desired: 316,
    ordering: .sequentiallyConsistent
  )
}

//===----------------------------------------------------------------------===//
// Dead Object Elimination
//===----------------------------------------------------------------------===//

// CHECK-LABEL: sil {{.*}} @deadAtomic {{.*}} {
// CHECK:         %0 = tuple ()
// CHECK-NEXT:    return %0 : $()
// CHECK-LABEL: } // end sil function 'deadAtomic'
@_silgen_name("deadAtomic")
func deadAtomic() {
  let _ = Atomic(0)
  let _ = Atomic<UnsafeRawPointer?>(nil)
}

//===----------------------------------------------------------------------===//
// Closure Lifetime Fixup
//===----------------------------------------------------------------------===//

func nonescapingClosure(with body: () -> ()) {
  body()
}

// CHECK-LABEL: sil {{.*}} @testNonescapingClosure {{.*}} {
// CHECK:         {{%.*}} = alloc_stack [lexical] [var_decl] $Atomic<Int>, let, name "foo"
// CHECK:         {{%.*}} = alloc_stack [lexical] [var_decl] $Atomic<Int>, let, name "bar"
// CHECK:         builtin "atomicrmw_add_monotonic_Int[[PTR_SIZE]]"
// CHECK:         builtin "atomicrmw_add_monotonic_Int[[PTR_SIZE]]"

// Make sure there are no moves
// CHECK-NOT:     alloc_stack $Atomic<Int>

// Make sure we don't emit a partial application
// CHECK-NOT:     partial_apply

// CHECK-LABEL: } // end sil function 'testNonescapingClosure'
@_silgen_name("testNonescapingClosure")
func testNonescapingClosure() {
  let foo = Atomic(0)
  let bar = Atomic(1)

  nonescapingClosure {
    foo.wrappingAdd(1, ordering: .relaxed)
    bar.wrappingAdd(1, ordering: .relaxed)
  }

  testInt(foo.load(ordering: .relaxed)) // OK
  testInt(bar.load(ordering: .relaxed)) // OK
}
