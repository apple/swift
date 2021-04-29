// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -profile-generate -profile-coverage-mapping -emit-sil -module-name coverage_closures %s | %FileCheck %s
// RUN: %target-swift-frontend -profile-generate -profile-coverage-mapping -emit-ir %s

func bar(arr: [(Int32) -> Int32]) {
// CHECK-LABEL: sil_coverage_map {{.*}}// closure #1 (Swift.Int32) -> Swift.Int32 in coverage_closures.bar
// CHECK-NEXT:  [[@LINE+1]]:13 -> [[@LINE+1]]:42 : 0
  for a in [{ (b : Int32) -> Int32 in b }] {
    a(0)
  }
}

func foo() {
// CHECK-LABEL: sil_coverage_map {{.*}}// closure #1 (Swift.Int32, Swift.Int32) -> Swift.Bool in coverage_closures.foo()
// CHECK-NEXT: [[@LINE+1]]:12 -> [[@LINE+1]]:59 : 0
  let c1 = { (i1 : Int32, i2 : Int32) -> Bool in i1 < i2 }

// CHECK-LABEL: sil_coverage_map {{.*}}// f1 #1 ((Swift.Int32, Swift.Int32) -> Swift.Bool) -> Swift.Bool in coverage_closures.foo()
// CHECK-NEXT: [[@LINE+1]]:55 -> {{.*}}:4 : 0
  func f1(_ closure : (Int32, Int32) -> Bool) -> Bool {
// CHECK-LABEL: sil_coverage_map {{.*}}// implicit closure #1 () async throws -> Swift.Bool in f1
// CHECK-NEXT: [[@LINE+1]]:29 -> [[@LINE+1]]:42 : 0
    return closure(0, 1) && closure(1, 0)
  }

  f1(c1)

// CHECK-LABEL: sil_coverage_map {{.*}}// closure #2 (Swift.Int32, Swift.Int32) -> Swift.Bool in coverage_closures.foo()
// CHECK-NEXT: [[@LINE+1]]:6 -> [[@LINE+1]]:27 : 0
  f1 { i1, i2 in i1 > i2 }

// CHECK-LABEL: sil_coverage_map {{.*}}// closure #3 (Swift.Int32, Swift.Int32) -> Swift.Bool in coverage_closures.foo()
// CHECK-NEXT: [[@LINE+3]]:6 -> [[@LINE+3]]:48 : 0
// CHECK-LABEL: sil_coverage_map {{.*}}// implicit closure #1 () async throws -> {{.*}} in coverage_closures.foo
// CHECK-NEXT: [[@LINE+1]]:36 -> [[@LINE+1]]:46 : 0
  f1 { left, right in left == 0 || right == 1 }
}

// SR-2615: Display coverage for implicit member initializers without crashing
struct C1 {
  // CHECK-LABEL: sil_coverage_map{{.*}}// variable initialization expression of coverage_closures.C1
  // CHECK-NEXT: [[@LINE+1]]:24 -> [[@LINE+1]]:34 : 0
  private var errors = [String]()
}

// rdar://39200851: Closure in init method covered twice

class C2 {
  init() {
// CHECK-LABEL: sil_coverage_map {{.*}}// closure #1 () -> () in coverage_closures.C2.init()
// CHECK-NEXT:  [[@LINE+2]]:13 -> [[@LINE+4]]:6 : 0
// CHECK-NEXT: }
    let _ = { () in
      print("hello")
    }
  }
}
