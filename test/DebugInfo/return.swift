// RUN: %target-swift-frontend %s -g -emit-ir -o - | %FileCheck %s

class X {
  init (i : Int64) { x = i }
  var x : Int64
}

// CHECK: define {{.*}}ifelseexpr
public func ifelseexpr() -> Int64 {
  var x = X(i:0)
  // CHECK: [[ALLOCA:%.*]] = alloca ptr
  // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s6return1XCMa"(
  // CHECK: [[META:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK: [[X:%.*]] = call {{.*}}ptr @"$s6return1XC1iACs5Int64V_tcfC"(
  // CHECK-SAME:                                  i64 0, ptr swiftself [[META]])
  // CHECK:  store ptr [[X]], ptr [[ALLOCA]]
  // CHECK:  @swift_release
  if true {
    x.x += 1
  } else {
    x.x -= 1
  }
  // CHECK:  [[L:%.*]] = load ptr, ptr [[ALLOCA]]
  // CHECK:  @swift_release
  // CHECK-SAME:                    , !dbg ![[RELEASE:.*]]
  // The ret instruction should be in the same scope as the return expression.
  // CHECK:  ret{{.*}}, !dbg ![[RELEASE]]
  return x.x // CHECK: ![[RELEASE]] = !DILocation(line: [[@LINE]], column: 3
}

