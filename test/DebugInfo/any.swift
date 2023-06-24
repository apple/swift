// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func markUsed<T>(_ t: T) {}

func main() {
	// CHECK: define hidden swiftcc void @"$s3any4mainyyF"
  // CHECK: call void @llvm.dbg.declare(metadata ptr {{.*}}, metadata ![[S:.*]], metadata !DIExpression()), !dbg ![[DBG:.*]]
  // CHECK: ![[S]] = !DILocalVariable(name: "s", {{.*}}line: [[@LINE+2]]
  // CHECK: ![[DBG]] = !DILocation(line: [[@LINE+1]], column: 7,
  var s: Any = "hello world"
  var n: Any = 12
  var t: Any = (1,2)
  markUsed("hello world")
}

main()
