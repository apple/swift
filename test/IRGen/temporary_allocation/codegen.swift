// RUN: %target-swift-frontend -primary-file %s -O -emit-ir | %FileCheck %s
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib

@_silgen_name("blackHole")
func blackHole(_ value: UnsafeMutableRawPointer?) -> Void

// MARK: Pointer width
do {
  let ptr = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
  blackHole(ptr)
  ptr.deallocate()
}

// MARK: Trivial Cases

// CHECK: [[ONE_BYTE_PTR_RAW:%temp_alloc[0-9]*]] = alloca i8, align 1
// CHECK: [[FIVE_BYTE_PTR_RAW:%temp_alloc[0-9]*]] = alloca [5 x i8], align 1
// CHECK: [[ONE_KB_PTR_RAW:%temp_alloc[0-9]*]] = alloca [1024 x i8], align 8
// CHECK: [[ONE_KB_RAND_PTR_RAW:%temp_alloc[0-9]*]] = alloca [1024 x i8], align 16
// CHECK: [[INT_PTR_RAW:%temp_alloc[0-9]*]] = alloca [16 x i8], align 4
// CHECK: [[INT_PTR_RAW2:%temp_alloc[0-9]*]] = alloca [16 x i8], align 4
// CHECK: [[VOID_PTR_RAW:%temp_alloc[0-9]*]] = alloca [2 x i8], align 1

// CHECK: ptrtoint ptr {{.*}} to [[WORD:i[0-9]+]]


withUnsafeTemporaryAllocation(byteCount: 0, alignment: 1) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[ZERO_BYTE_PTR_RAW:%temp_alloc[0-9]*]] = alloca i8, align 1
// CHECK: [[ZERO_BYTE_PTR:%[0-9]+]] = ptrtoint ptr [[ZERO_BYTE_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[ZERO_BYTE_PTR]])

withUnsafeTemporaryAllocation(byteCount: 1, alignment: 1) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[ONE_BYTE_PTR:%[0-9]+]] = ptrtoint ptr [[ONE_BYTE_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[ONE_BYTE_PTR]])

withUnsafeTemporaryAllocation(byteCount: 5, alignment: 1) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[FIVE_BYTE_PTR:%[0-9]+]] = ptrtoint ptr [[FIVE_BYTE_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[FIVE_BYTE_PTR]])

withUnsafeTemporaryAllocation(byteCount: 1024, alignment: 8) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[ONE_KB_PTR:%[0-9]+]] = ptrtoint ptr [[ONE_KB_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[ONE_KB_PTR]])

// MARK: Alignment unknown at compile-time

withUnsafeTemporaryAllocation(byteCount: 1024, alignment: Int.random(in: 0 ..< 16)) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[ONE_KB_RAND_PTR:%[0-9]+]] = ptrtoint ptr [[ONE_KB_RAND_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[ONE_KB_RAND_PTR]])

// MARK: Typed buffers

withUnsafeTemporaryAllocation(of: Int32.self, capacity: 4) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[INT_PTR:%[0-9]+]] = ptrtoint ptr [[INT_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[INT_PTR]])

_withUnprotectedUnsafeTemporaryAllocation(of: Int32.self, capacity: 4) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[INT_PTR:%[0-9]+]] = ptrtoint ptr [[INT_PTR_RAW2]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[INT_PTR]])

withUnsafeTemporaryAllocation(of: Void.self, capacity: 2) { buffer in
  blackHole(buffer.baseAddress)
}
// CHECK: [[VOID_PTR:%[0-9]+]] = ptrtoint ptr [[VOID_PTR_RAW]] to [[WORD]]
// CHECK: call swiftcc void @blackHole([[WORD]] [[VOID_PTR]])

