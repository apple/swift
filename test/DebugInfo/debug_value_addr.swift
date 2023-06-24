// RUN: %target-swift-frontend -primary-file %s -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend %s -emit-sil -g -o - | %FileCheck -check-prefix=CHECK-SIL %s

// Verify that -Onone shadow copies are emitted for debug_value_addr
// instructions.

// CHECK-SIL: sil hidden @$s16debug_value_addr4testyyxlF
// CHECK-SIL: debug_value %0 : $*T, let, name "t", {{.*}}, expr op_deref

// CHECK: define {{.*}}$s16debug_value_addr4testyyxlF
// CHECK: entry:
// CHECK-NEXT: %[[TADDR:.*]] = alloca
// CHECK-NEXT: call void @llvm.dbg.declare({{.*}}%[[TADDR]]
// CHECK: store ptr %0, ptr %[[TADDR:.*]], align

struct S<T> {
  var a : T
  func foo() {}
}

func test<T>(_ t : T) {
  let a = S(a: t)
  a.foo()
}

func use<T>(_ t : T) {}

// CHECK-SIL: sil hidden @$s16debug_value_addr11GenericSelfV1xACyxGx_tcfC : $@convention(method) <T> (@in T, @thin GenericSelf<T>.Type) -> GenericSelf<T> {
// CHECK-SIL: bb0(%0 : $*T, %1 : $@thin GenericSelf<T>.Type):
//
// CHECK-SIL-NEXT:  alloc_stack $GenericSelf<T>, var, name "self", implicit, loc {{.*}}
// CHECK-SIL-NEXT:  debug_value %0 : $*T, let, name "x", argno 1, expr op_deref, loc {{.*}}
struct GenericSelf<T> {
  init(x: T) {
    // 'self' is a valid debug variable here even though there is
    // nothing to initialize (the dead alloc_stack cannot be removed).
    use(x)
  }
}
