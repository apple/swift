// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx13.0 -module-name main -emit-ir -o %t/new.ir
// RUN: %FileCheck %s --check-prefix=NEW < %t/new.ir

// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx12.0 -module-name main -emit-ir -o %t/backdeploy_56.ir
// RUN: %FileCheck %s --check-prefix=BACKDEPLOY56 < %t/backdeploy_56.ir

// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx10.15 -module-name main -emit-ir -o %t/backdeployed_concurrency.ir -disable-availability-checking
// RUN: %FileCheck %s --check-prefixes=BACKDEPLOY_CONCURRENCY,BACKDEPLOY56 < %t/backdeployed_concurrency.ir

// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx10.15 -O -module-name main -emit-ir -o %t/optimized.ir -disable-availability-checking
// RUN: %FileCheck %s --check-prefix=OPTIMIZED < %t/optimized.ir


// REQUIRES: OS=macosx

// NEW-NOT: extern_weak

// BACKDEPLOY_CONCURRENCY: @"$sScPMn" = extern_weak global
// BACKDEPLOY_CONCURRENCY: declare extern_weak swiftcc ptr @swift_task_alloc
// BACKDEPLOY_CONCURRENCY: declare extern_weak swiftcc %swift.metadata_response @"$sScPMa"

// BACKDEPLOY_CONCURRENCY: declare extern_weak void @"_swift_FORCE_LOAD_$_swiftCompatibilityConcurrency"()
// BACKDEPLOY56: declare extern_weak void @"_swift_FORCE_LOAD_$_swiftCompatibility56"()

// BACKDEPLOY_CONCURRENCY: declare extern_weak swiftcc i8 @"$sScP8rawValues5UInt8Vvg"

// OPTIMIZED: @swift_async_extendedFramePointerFlags = extern_weak global ptr
// OPTIMIZED: @_swift_async_extendedFramePointerFlagsUser = linkonce_odr hidden global ptr @swift_async_extendedFramePointerFlags
// OPTIMIZED: @llvm.used =
// OPTIMIZED-SAME: ptr @_swift_async_extendedFramePointerFlagsUser

@available(macOS 12.0, *)
public func g() async -> String { "hello" }

@available(macOS 12.0, *)
public func f() async {
  Task {
    print(await g())
  }
}
