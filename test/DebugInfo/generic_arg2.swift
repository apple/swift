// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

// CHECK: define hidden swiftcc void @"$s12generic_arg25ClassC3foo{{.*}}, ptr %U
// CHECK: call void @llvm.dbg.declare(metadata ptr %y.debug, metadata ![[U:.*]], metadata !DIExpression(DW_OP_deref))
// Make sure there is no conflicting dbg.value for this variable.x
// CHECK-NOT: dbg.value{{.*}}metadata ![[U]]
class Class <T> {
// CHECK: ![[U]] = !DILocalVariable(name: "y", arg: 2{{.*}} line: [[@LINE+1]],
  func foo<U>(_ x: T, y: U) {}

  func bar(_ x: String, y: Int64) {}

  init() {}
}

func main() {
  var object: Class<String> = Class()
  var x = "hello"
  var y : Int64 = 1234
  object.bar(x, y: y)
  object.foo(x, y: y)
}

main()
