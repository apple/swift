// RUN: %target-swift-frontend -module-name=test -emit-sil %s -O | %FileCheck %s --check-prefix=CHECK --check-prefix=DEFAULT
// RUN: %target-swift-frontend -module-name=test -enable-move-inout-stack-protector -emit-sil %s -O -enable-stack-protector | %FileCheck %s --check-prefix=CHECK --check-prefix=MOVE

// REQUIRES: swift_in_compiler

@_silgen_name("potentiallyBadCFunction")
func potentiallyBadCFunction(_ arg: UnsafePointer<Int>)

// CHECK-LABEL: sil [stack_protection] @$s4test20overflowInCFunction1yyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test20overflowInCFunction1yyF'
public func overflowInCFunction1() {
  var x = 0
  withUnsafeMutablePointer(to: &x) {
    potentiallyBadCFunction($0)
  }
}

// CHECK-LABEL: sil @$s4test19unprotectedOverflowyyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test19unprotectedOverflowyyF'
public func unprotectedOverflow() {
  var x = 0
  _withUnprotectedUnsafeMutablePointer(to: &x) {
    potentiallyBadCFunction($0)
  }
}

// CHECK-LABEL: sil [stack_protection] @$s4test23overflowWithUnsafeBytesyyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test23overflowWithUnsafeBytesyyF'
public func overflowWithUnsafeBytes() {
  var x = 0
  withUnsafeBytes(of: &x) {
    potentiallyBadCFunction($0.bindMemory(to: Int.self).baseAddress!)
  }
}

// CHECK-LABEL: sil [stack_protection] @$s4test31owerflowWithUnsafeBorrowedBytes5valueySi_tF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test31owerflowWithUnsafeBorrowedBytes5valueySi_tF'
public func owerflowWithUnsafeBorrowedBytes(value: Int) {
  withUnsafeBytes(of: value) {
    potentiallyBadCFunction($0.bindMemory(to: Int.self).baseAddress!)
  }
}

// CHECK-LABEL: sil @$s4test9onlyLoads5valueS2i_tF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test9onlyLoads5valueS2i_tF'
public func onlyLoads(value: Int) -> Int {
  withUnsafeBytes(of: value) {
    $0.load(as: Int.self)
  }
}

// CHECK-LABEL: sil @$s4test22unprotectedUnsafeBytesyyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test22unprotectedUnsafeBytesyyF'
public func unprotectedUnsafeBytes() {
  var x = 0
  _withUnprotectedUnsafeBytes(of: &x) {
    potentiallyBadCFunction($0.bindMemory(to: Int.self).baseAddress!)
  }
}

// CHECK-LABEL: sil [stack_protection] @$s4test20overflowInCFunction2yyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test20overflowInCFunction2yyF'
public func overflowInCFunction2() {
  var x = 0
  potentiallyBadCFunction(&x)
}

// CHECK-LABEL: sil hidden [noinline] @$s4test20inoutWithKnownCalleryySizF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test20inoutWithKnownCalleryySizF'
@inline(never)
func inoutWithKnownCaller(_ x: inout Int) {
  withUnsafeMutablePointer(to: &x) {
    $0[1] = 0
  }
}

// CHECK-LABEL: sil [stack_protection] @$s4test24callOverflowInoutPointeryyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test24callOverflowInoutPointeryyF'
public func callOverflowInoutPointer() {
  var x = 27
  inoutWithKnownCaller(&x)
}

// DEFAULT-LABEL: sil @$s4test22inoutWithUnknownCalleryySizF
// MOVE-LABEL:    sil [stack_protection] @$s4test22inoutWithUnknownCalleryySizF
// MOVE:            copy_addr [take] {{.*}} to [init]
// MOVE:            copy_addr [take] {{.*}} to [init]
// DEFAULT-NOT:     copy_addr
// CHECK:         } // end sil function '$s4test22inoutWithUnknownCalleryySizF'
public func inoutWithUnknownCaller(_ x: inout Int) {
  withUnsafeMutablePointer(to: &x) {
    $0[1] = 0
  }
}

// CHECK-LABEL: sil [stack_protection] @$s4test0A29WithUnsafeTemporaryAllocationyyF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test0A29WithUnsafeTemporaryAllocationyyF'
public func testWithUnsafeTemporaryAllocation() {
  withUnsafeTemporaryAllocation(of: Int.self, capacity: 10) {
    potentiallyBadCFunction($0.baseAddress!)
  }
}

// CHECK-LABEL: sil @$s4test13loadUnalignedySiSVF
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test13loadUnalignedySiSVF'
public func loadUnaligned(_ urp: UnsafeRawPointer) -> Int {
  return urp.loadUnaligned(as: Int.self)
}

// CHECK-LABEL: sil @$s4test19storeBytesToPointeryySv_SitF :
// CHECK-NOT:     copy_addr
// CHECK:       } // end sil function '$s4test19storeBytesToPointeryySv_SitF'
public func storeBytesToPointer(_ p: UnsafeMutableRawPointer, _ i: Int) {
  p.storeBytes(of: i, as: Int.self)
}

