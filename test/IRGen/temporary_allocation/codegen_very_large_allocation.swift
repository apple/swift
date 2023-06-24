// RUN: %target-swift-frontend -primary-file %s -O -emit-ir | %FileCheck %s --check-prefixes=CHECK-LARGE-ALLOC,CHECK-LARGE-ALLOC-%target-vendor -DWORD=i%target-ptrsize
// RUN: %target-swift-frontend -primary-file %s -O -emit-ir | %FileCheck %s --check-prefix=CHECK-LARGE-STACK-ALLOC -DWORD=i%target-ptrsize
// RUN: %target-swift-frontend -primary-file %s -O -emit-ir | %FileCheck %s --check-prefix=CHECK-LARGE-HEAP-ALLOC -DWORD=i%target-ptrsize

@_silgen_name("blackHole")
func blackHole(_ value: UnsafeMutableRawPointer?) -> Void

// MARK: Very large allocation

// A large allocation size should produce an OS version check, call to
// swift_stdlib_isStackAllocationSafe(), and then a branch based on the result
// to either stack-allocate or heap-allocate.
withUnsafeTemporaryAllocation(byteCount: 0x0FFF_FFFF, alignment: 1) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK-LARGE-HEAP-ALLOC: [[HEAP_PTR_RAW:%[0-9]+]] = {{(tail )?}}call noalias ptr @swift_slowAlloc([[WORD]] 268435455, [[WORD]] -1)
// CHECK-LARGE-HEAP-ALLOC-NEXT: [[HEAP_PTR:%[0-9]+]] = ptrtoint ptr [[HEAP_PTR_RAW]] to [[WORD]]
// CHECK-LARGE-HEAP-ALLOC-NEXT: {{(tail )?}}call swiftcc void @blackHole([[WORD]] [[HEAP_PTR]])
// CHECK-LARGE-HEAP-ALLOC-NEXT: {{(tail )?}}call void @swift_slowDealloc(ptr [[HEAP_PTR_RAW]], [[WORD]] -1, [[WORD]] -1)

// CHECK-LARGE-STACK-ALLOC: [[STACK_PTR_RAW:%temp_alloc[0-9]*]] = alloca [268435455 x i8], align 1
// CHECK-LARGE-STACK-ALLOC-NEXT: [[STACK_PTR:%[0-9]+]] = ptrtoint ptr [[STACK_PTR_RAW]] to [[WORD]]
// CHECK-LARGE-STACK-ALLOC-NEXT: call swiftcc void @blackHole([[WORD]] [[STACK_PTR]])

// CHECK-LARGE-ALLOC-DAG: [[IS_SAFE:%[0-9]+]] = {{(tail )?}}call {{(zeroext )?}}i1 @swift_stdlib_isStackAllocationSafe([[WORD]] 268435455, [[WORD]] 1)
// CHECK-LARGE-ALLOC-DAG: br i1 [[IS_SAFE]], label %{{[0-9]+}}, label %{{[0-9]+}}
// CHECK-LARGE-ALLOC-apple-DAG: [[IS_OS_OK:%[0-9]+]] = {{(tail )?}}call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"
// CHECK-LARGE-ALLOC-apple-DAG: br i1 [[IS_OS_OK]], label %{{[0-9]+}}, label %{{[0-9]+}}
