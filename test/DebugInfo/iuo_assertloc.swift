// RUN: %target-swift-frontend %s -emit-sil -parse-as-library -g -o - | %FileCheck %s --check-prefix=CHECK-SIL
// RUN: %target-swift-frontend %s -emit-ir -parse-as-library -g -o - | %FileCheck %s --check-prefix=CHECK-IR

public func f(_ a: Int!) -> Int {
  // CHECK-IR: call {{.*}}assertionFailure{{.*}}, !dbg ![[LOC:[0-9]+]]
  // CHECK-IR: ![[LOC]] = !DILocation(line: [[@LINE+1]], column: 11
  return (a // CHECK-SIL: apply {{.*}}:[[@LINE]]:11
          +
          1)
}
