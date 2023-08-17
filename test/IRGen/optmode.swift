// RUN: %target-swiftc_driver %s -emit-ir -O | %FileCheck %s 

// CHECK: @"$s7optmode7square11nS2i_tF"{{.*}}[[ATTR1:#[0-9]+]]
@_optimize(none)
func square1(n: Int) -> Int {
    return n * n
}

// CHECK: @"$s7optmode7square21nS2i_tF"{{.*}}[[ATTR2:#[0-9]+]]
@_optimize(size)
func square2(n: Int) -> Int {
    return n * n
}

// CHECK: attributes [[ATTR1]] = { {{.*}}noinline optnone{{.*}} }
// CHECK: attributes [[ATTR2]] = { {{.*}}optsize{{.*}} }
