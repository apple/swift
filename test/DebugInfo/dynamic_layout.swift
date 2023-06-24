// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func markUsed<T>(_ t: T) {}

class Class <T> {
  var x: T

  init(_x : T) {x = _x}

  // Verify that the mangling of the type U is correct.
  // CHECK: define {{.*}}3foo
  // CHECK: %[[U1:.*]] = alloca ptr
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[U1]],
  // CHECK-SAME:                        metadata ![[U:[0-9]+]]
  // CHECK: %[[T2:.*]] = alloca ptr
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[T2]],
  // CHECK-SAME:                        metadata ![[T:[0-9]+]]
  // CHECK: ![[U]] = !DILocalVariable(name: "$\CF\84_1_0"
  // CHECK: ![[T]] = !DILocalVariable(name: "$\CF\84_0_0"
  func foo <U> (_ y : U) -> (T,U) {
    var tuple = (x,y)
    return tuple
  }
}

func main() {
  var v = Class<Int64>(_x: 1)
  var tuple = v.foo("hi")
  markUsed(tuple.0)
  markUsed(tuple.1)
}

main()
