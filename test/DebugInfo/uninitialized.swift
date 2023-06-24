// RUN: %target-swift-frontend %s -c -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend %s -O -c -emit-ir -g -o - | %FileCheck %s --check-prefix=OPT
class MyClass {}

// CHECK-LABEL: define {{.*}} @"$s13uninitialized1fyyF"
// OPT-LABEL: define {{.*}} @"$s13uninitialized1fyyF"
public func f() {
  var object: MyClass
  // CHECK: %[[OBJ:.*]] = alloca ptr, align
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[OBJ]],
  // CHECK: void @llvm.memset.{{.*}}(ptr align {{(4|8)}} %[[OBJ]], i8 0,
  // CHECK-SAME:                    ){{$}}
  // OPT-NOT: @llvm.memset
  // OPT: ret
}

// CHECK-LABEL: define {{.*}} @"$s13uninitialized1gyyF"
// OPT-LABEL: define {{.*}} @"$s13uninitialized1gyyF"
public func g() {
  var dict: Dictionary<Int64, Int64>
  // CHECK: %[[DICT:.*]] = alloca
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[DICT]],
  // CHECK: void @llvm.memset.{{.*}}(ptr align {{(4|8)}} %[[DICT]], i8 0,
  // CHECK-SAME:                    ){{$}}
  // OPT-NOT: @llvm.memset
  // OPT: ret
}
