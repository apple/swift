// RUN: %target-swift-emit-silgen -enable-experimental-feature RawLayout %s | %FileCheck %s

// CHECK: @_rawLayout(size: 4, alignment: 4) @_moveOnly struct Lock
// CHECK: @_rawLayout(like: T) @_moveOnly struct Cell<T>
// CHECK: @_rawLayout(likeArrayOf: T, count: 8) @_moveOnly struct SmallVectorBuf<T>

@_rawLayout(size: 4, alignment: 4)
struct Lock: ~Copyable {
    // Raw layout type should be lowered as address only
    // CHECK-LABEL: sil {{.*}} @{{.*}}4Lock{{.*}}3foo{{.*}} : $@convention(method) (@in_guaranteed Lock) -> ()
    borrowing func foo() {}

    // CHECK-LABEL: sil {{.*}} @{{.*}}4Lock{{.*}}3bar{{.*}} : $@convention(method) (@inout Lock) -> ()
    mutating func bar() {}

    // CHECK-LABEL: sil {{.*}} @{{.*}}4Lock{{.*}}3bas{{.*}} : $@convention(method) (@in Lock) -> ()
    consuming func bas() {}

    deinit {}
}

@_rawLayout(like: T)
struct Cell<T>: ~Copyable {}

@_rawLayout(likeArrayOf: T, count: 8)
struct SmallVectorBuf<T>: ~Copyable {}
