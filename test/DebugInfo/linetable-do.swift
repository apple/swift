// RUN: %target-swift-frontend -Xllvm -sil-full-demangle %s -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -disable-copy-propagation %s -emit-sil -emit-verbose-sil -g -o - | %FileCheck --check-prefixes=CHECK-SIL,CHECK-NCP %s
// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -enable-copy-propagation %s -emit-sil -emit-verbose-sil -g -o - | %FileCheck --check-prefixes=CHECK-SIL,CHECK-CP %s
import StdlibUnittest

class Obj {}

func foo (_ a : Int64) throws -> Void {
  _blackHole(a)
}

// CHECK-SIL: // main.testDoStmt() throws -> ()
func testDoStmt() throws -> Void {
  _blackHole(23)

  // CHECK-NOT: !DILocation(line: [[@LINE+1]]
  do {
    let obj = Obj()
    _blackHole(obj)
    // CHECK-CP:  destroy_value [poison] %{{.*}} : $Obj{{.*}} line:[[@LINE+1]]:13:auto_gen
    try foo(100)
    // CHECK-SIL: bb{{.*}}(%{{[0-9]+}} : $()):
    // CHECK-NCP-NEXT: strong_release {{.*}}: $Obj{{.*}} line:[[@LINE+1]]:3:cleanup
  }
  // CHECK-SIL-NEXT:     = tuple ()
  // CHECK-SIL-NEXT:   return                        {{.*}} line:[[@LINE+1]]
}

try testDoStmt()

// CHECK-SIL: // main.testDoWhileStmt() -> ()
func testDoWhileStmt() -> Void {
  // CHECK-NOT: !DILocation(line: [[@LINE+1]]
  do {
    try foo(100)
    // CHECK-SIL: bb{{.*}}(%{{[0-9]+}} : $()):
    // CHECK-SIL-NEXT:  br [[RETURN_BB:bb[0-9]+]]{{.*}} line:[[@LINE+1]]:3:cleanup
  }
  // CHECK-SIL: [[RETURN_BB]]:
  // CHECK-SIL-NEXT:     = tuple ()
  // CHECK-SIL-NEXT:   return

  catch (let e) {
    _blackHole(e)
  }
}
