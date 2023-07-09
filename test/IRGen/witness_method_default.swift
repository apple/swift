// RUN: %target-swift-frontend %s -emit-ir | %FileCheck --check-prefix=CHECK %s -DINT=i%target-ptrsize

public protocol DummyProtocol { }

public protocol SIMDStorageStub {
 associatedtype Scalar : DummyProtocol
}

public protocol SIMDScalarStub {
 associatedtype SIMD2Storage : SIMDStorageStub
   where SIMD2Storage.Scalar == Self
 
 func abs() -> Self
}

// CHECK: define {{.*}}swiftcc void @"$s22witness_method_default7callAbs1sxx_tAA14SIMDScalarStubRzlF
public func callAbs<T: SIMDScalarStub>(s: T) -> T {
  // CHECK: [[ABS_PTR:%[0-9]+]] = getelementptr inbounds ptr, ptr %T.SIMDScalarStub, i32 3
  // CHECK-NEXT: [[ABS:%[0-9]+]] = load ptr, ptr [[ABS_PTR]]
  // CHECK: call swiftcc void [[ABS]]
 return s.abs()
}
