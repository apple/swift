// RUN: %target-swift-frontend -parse-stdlib %s -emit-ir | %FileCheck %s

import Swift
import _Differentiation

struct ExamplePullbackStruct<T: Differentiable> {
  var pb0: (T.TangentVector) -> T.TangentVector
}

@_silgen_name("test_context_builtins")
func test_context_builtins() {
  let pbStruct = ExamplePullbackStruct<Float>(pb0: { $0 })
  let context = Builtin.autoDiffCreateLinearMapContext(Builtin.sizeof(type(of: pbStruct)))
  let topLevelSubctxAddr = Builtin.autoDiffProjectTopLevelSubcontext(context)
  UnsafeMutableRawPointer(topLevelSubctxAddr).storeBytes(of: pbStruct, as: type(of: pbStruct))
  let newBuffer = Builtin.autoDiffAllocateSubcontext(context, Builtin.sizeof(type(of: pbStruct)))
  UnsafeMutableRawPointer(newBuffer).storeBytes(of: pbStruct, as: type(of: pbStruct))
}

// CHECK-LABEL: define{{.*}}@test_context_builtins()
// CHECK: entry:
// CHECK:   [[CTX:%.*]] = call swiftcc ptr @swift_autoDiffCreateLinearMapContext({{i[0-9]+}} {{.*}})
// CHECK:   call swiftcc ptr @swift_autoDiffProjectTopLevelSubcontext(ptr [[CTX]])
// CHECK:   [[BUF:%.*]] = call swiftcc ptr @swift_autoDiffAllocateSubcontext(ptr [[CTX]], {{i[0-9]+}} {{.*}})
