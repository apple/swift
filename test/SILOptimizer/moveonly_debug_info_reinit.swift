// RUN: %target-swift-frontend -emit-sil -g %s | %FileCheck %s
// RUN: %target-swift-frontend -DADDRESS_ONLY -emit-sil -g %s | %FileCheck %s

struct Foo: ~Copyable {
#if ADDRESS_ONLY
    private var x: Any
#else
    private var x: Int
#endif
}

@_silgen_name("use")
func use(_: inout Foo)

// CHECK-LABEL: sil {{.*}} @${{.*}}3bar
func bar(_ x: consuming Foo, y: consuming Foo, z: consuming Foo) {
    // CHECK: [[X:%.*]] = alloc_stack{{.*}} $Foo, var, name "x"

    // CHECK: [[USE:%.*]] = function_ref @use
    // CHECK: apply [[USE]]
    // CHECK: debug_value undef : $*Foo, var, name "x"
    use(&x)
    let _ = x

    // CHECK: debug_value undef : $*Foo, var, name "y"
    // CHECK: debug_value [[X]] : $*Foo, var, name "x"
    x = y
    // CHECK: [[USE:%.*]] = function_ref @use
    // CHECK: apply [[USE]]
    // CHECK: debug_value undef : $*Foo, var, name "x"
    use(&x)
    let _ = x

    // CHECK: debug_value undef : $*Foo, var, name "z"
    // CHECK: debug_value [[X]] : $*Foo, var, name "x"
    x = z
    // CHECK: [[USE:%.*]] = function_ref @use
    // CHECK: apply [[USE]]
    // CHECK: debug_value undef : $*Foo, var, name "x"
    use(&x)
}

// CHECK-LABEL: sil {{.*}} @${{.*}}10inoutParam
func inoutParam(_ x: inout Foo, y: consuming Foo, z: consuming Foo) {
    // CHECK: debug_value [[X:%[0-9]+]] : $*Foo, var, name "x"

    // CHECK: [[USE:%.*]] = function_ref @use
    // CHECK: apply [[USE]]
    // CHECK: debug_value undef : $*Foo, var, name "x"
    use(&x)
    let _ = x

    // CHECK: debug_value undef : $*Foo, var, name "y"
    // CHECK: debug_value [[X]] : $*Foo, var, name "x"
    x = y
    // CHECK: [[USE:%.*]] = function_ref @use
    // CHECK: apply [[USE]]
    // CHECK: debug_value undef : $*Foo, var, name "x"
    use(&x)
    let _ = x

    // CHECK: debug_value undef : $*Foo, var, name "z"
    // CHECK: debug_value [[X]] : $*Foo, var, name "x"
    x = z
}
