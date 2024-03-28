// RUN: %target-swift-emit-silgen -enable-experimental-feature RawLayout -enable-builtin-module %s | %FileCheck %s

// XFAIL: noncopyable_generics

// CHECK: @_rawLayout(size: 4, alignment: 4) @_moveOnly struct Lock
// CHECK: @_rawLayout(like: T) @_moveOnly struct Cell<T>
// CHECK: @_rawLayout(likeArrayOf: T, count: 8) @_moveOnly struct SmallVectorBuf<T>

import Builtin

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
struct Cell<T>: ~Copyable {
    // CHECK-LABEL: sil {{.*}} @$s10raw_layout4CellV7addressSpyxGvg : $@convention(method) <T> (@in_guaranteed Cell<T>) -> UnsafeMutablePointer<T> {
    // CHECK:         {{%.*}} = builtin "addressOfRawLayout"<Cell<T>>({{%.*}} : $*Cell<T>) : $Builtin.RawPointer
    // CHECK-LABEL: } // end sil function '$s10raw_layout4CellV7addressSpyxGvg'
    var address: UnsafeMutablePointer<T> {
        .init(Builtin.addressOfRawLayout(self))
    }

    init(_ value: consuming T) {
        address.initialize(to: value)
    }
}

@_rawLayout(likeArrayOf: T, count: 8)
struct SmallVectorBuf<T>: ~Copyable {}
