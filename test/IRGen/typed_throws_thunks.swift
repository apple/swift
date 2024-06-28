// RUN: %target-swift-frontend -primary-file %s -emit-irgen -disable-availability-checking -runtime-compatibility-version none -target %module-target-future -enable-library-evolution | %FileCheck %s --check-prefix=CHECK

// REQUIRES: PTRSIZE=64
// UNSUPPORTED: CPU=arm64e

@frozen
public struct FixedSize : Error {
   var x = 0
   var y = 0
}

public protocol P {
  associatedtype Failure: Error = any Error

  func f(body: () throws -> Void) async rethrows
  func f2(body: () throws -> Void) rethrows

  func g(body: () throws(Failure) -> Void) async throws(Failure)
  func g4(body: () throws(FixedSize) -> Void) async throws(FixedSize)

  func g2(body: () throws(Failure) -> Void) throws(Failure)
  func g3(body: () throws(FixedSize) -> Void) throws(FixedSize)
}

extension P {
 // CHECK-LABEL: define{{.*}} swifttailcc void @"$s19typed_throws_thunks1PP1g4bodyyyy7FailureQzYKXE_tYaAGYKFTj"(ptr swiftasync %0, ptr %1, ptr %2, ptr noalias swiftself %3, ptr %4, ptr %5, ptr %6)
 // CHECK-NOT: ret
 // CHECK:  call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s({{.*}}, ptr %1, ptr %2, ptr %3, ptr %4, ptr %5, ptr %6)
  public func g(body: () throws(Failure) -> Void) async throws(Failure) {
    do {
      return try await f(body: body)
    } catch {
      throw error as! Failure
    }
  }

  // CHECK-LABEL: define{{.*}} swifttailcc void @"$s19typed_throws_thunks1PP2g44bodyyyyAA9FixedSizeVYKXE_tYaAGYKFTj"(ptr swiftasync %0, ptr %1, ptr %2, ptr noalias swiftself %3, ptr %4, ptr %5, ptr %6)
	// CHECK-NOT: ret
	// CHECK: call { ptr, ptr } (i32, ptr, ptr, ...) @llvm.coro.suspend.async.sl_p0p0s({{.*}} ptr %1, ptr %2, ptr %3, ptr %4, ptr %5, ptr %6)
  public func g4(body: () throws(FixedSize) -> Void) async throws(FixedSize) {
    do {
      return try await f(body: body)
    } catch {
      throw error as! FixedSize
    }
  }

  // CHECK-LABEL: define{{.*}} swiftcc void @"$s19typed_throws_thunks1PP2g24bodyyyy7FailureQzYKXE_tAGYKFTj"(ptr %0, ptr %1, ptr noalias swiftself %2, ptr noalias nocapture swifterror dereferenceable(8) %3, ptr %4, ptr %5, ptr %6)
	// CHECK-NOT: ret
  // CHECK: call swiftcc void {{.*}}(ptr %0, ptr %1, ptr noalias swiftself %2, ptr noalias nocapture swifterror dereferenceable(8) %3, ptr %4, ptr %5, ptr %6)

  public func g2(body: () throws(Failure) -> Void) throws(Failure) {
    do {
      return try f2(body: body)
    } catch {
      throw error as! Failure
    }
  }

  // CHECK-LABEL: define{{.*}} swiftcc { i64, i64 } @"$s19typed_throws_thunks1PP2g34bodyyyyAA9FixedSizeVYKXE_tAGYKFTj"(ptr %0, ptr %1, ptr noalias swiftself %2, ptr noalias nocapture swifterror dereferenceable(8) %3, ptr %4, ptr %5)
  // CHECK-NOT: ret
  // CHECK:  call swiftcc { i64, i64 } {{.*}}(ptr %0, ptr %1, ptr noalias swiftself %2, ptr noalias nocapture swifterror dereferenceable(8) %3, ptr %4, ptr %5)


  public func g3(body: () throws(FixedSize) -> Void) throws(FixedSize) {
  }
}
