// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-objc-interop -disable-objc-attr-requires-foundation-module -import-objc-header %S/Inputs/c_functions.h -primary-file %s -emit-ir | %FileCheck %s -check-prefix CHECK -check-prefix %target-cpu -check-prefix %target-abi-%target-cpu

// This is deliberately not a SIL test so that we can test SILGen too.

// CHECK-LABEL: define hidden swiftcc void @"$s11c_functions14testOverloadedyyF"
func testOverloaded() {
  // CHECK: call void @{{_Z10overloadedv|"\?overloaded@@\$\$J0YAXXZ"}}()
  overloaded()
  // CHECK: call void @{{_Z10overloadedi|"\?overloaded@@\$\$J0YAXH@Z"}}(i32{{( signext)?}} 42)
  overloaded(42)
  // CHECK: call void @{{.*}}test_my_log
  test_my_log()
} // CHECK: {{^}$}}

func test_indirect_by_val_alignment() {
  let x = a_thing()
  log_a_thing(x)
}

// We only want to test x86_64.
// x86_64-LABEL: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// x86_64: %indirect-temporary = alloca %TSo7a_thinga, align [[ALIGN:[0-9]+]]
// SYSV-x86_64: call void @log_a_thing(ptr byval({{.*}}) align [[ALIGN]] %indirect-temporary)
// WIN-x86_64: call void @log_a_thing(ptr %indirect-temporary)
// x86_64: define internal void @log_a_thing(ptr {{(byval(%struct.a_thing) align [[ALIGN]])?}}

// aarch64: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// arm64_32: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// arm64: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// arm64e: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// armv7k: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// armv7s: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// armv7: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// i386: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// s390x: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
// powerpc64le: define hidden swiftcc void  @"$s11c_functions30test_indirect_by_val_alignmentyyF"()
