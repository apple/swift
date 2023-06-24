// RUN: %target-swift-frontend -module-name A -emit-ir -primary-file %s -import-objc-header %S/Inputs/noreturn.h | %FileCheck %s


// CHECK-LABEL: define {{.*}} void @"$s1A018testDirectReturnNoC0yyF"()
// CHECK:   call i32 @scalarNoReturn()
// CHECK:   unreachable
public func testDirectReturnNoReturn() {
  scalarNoReturn()
}

// CHECK-LABEL: define {{.*}} void @"$s1A019testDirect2ReturnNoC0yyF"()
// CHECK:   call {{.*}}@smallStructNoReturn({{.*}})
// CHECK:   unreachable
public func testDirect2ReturnNoReturn() {
  smallStructNoReturn()
}

// CHECK-LABEL: define {{.*}} void @"$s1A020testIndirectReturnNoC0yyF"()
// CHECK:   [[INDIRECT_RESULT:%.*]] = alloca %struct.Large
// CHECK:   call void @largeStructNoReturn(ptr {{.*}}[[INDIRECT_RESULT]])
// CHECK:   unreachable
public func testIndirectReturnNoReturn() {
  largeStructNoReturn()
}

// CHECK-LABEL: define {{.*}} void @"$s1A016testVoidReturnNoC0yyF"()
// CHECK:   call void @voidNoReturn()
// CHECK:   unreachable
public func testVoidReturnNoReturn() {
  voidNoReturn()
}
