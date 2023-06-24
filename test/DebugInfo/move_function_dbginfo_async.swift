// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -parse-as-library -disable-availability-checking -g -emit-sil -o - %s | %FileCheck -check-prefix=SIL %s
// RUN: %target-swift-frontend -parse-as-library -disable-availability-checking -g -emit-ir -o - %s | %FileCheck %s
// RUN: %target-swift-frontend -parse-as-library -disable-availability-checking -g -c %s -o %t/out.o

// This test checks that:
//
// 1. At the IR level, we insert the appropriate llvm.dbg.addr, llvm.dbg.value.
//
// 2. At the Dwarf that we have proper locations with PC validity ranges where
//    the value isn't valid.

// We only run this on macOS right now since we would need to pattern match
// slightly differently on other platforms.
// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64 || CPU=arm64

//////////////////
// Declarations //
//////////////////

public class Klass {
    public func doSomething() {}
}

public protocol P {
    static var value: P { get }
    func doSomething()
}

public var trueValue: Bool { true }
public var falseValue: Bool { false }

public func use<T>(_ t: T) {}
public func forceSplit() async {}
public func forceSplit1() async {}
public func forceSplit2() async {}
public func forceSplit3() async {}
public func forceSplit4() async {}
public func forceSplit5() async {}

///////////
// Tests //
///////////

// CHECK-LABEL: define swifttailcc void @"$s27move_function_dbginfo_async13letSimpleTestyyxnYalF"(ptr swiftasync %0, ptr noalias %1, ptr %T)
// CHECK: entry:
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[SIMPLE_TEST_METADATA:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:   musttail call swifttailcc void
// CHECK-NEXT: ret void

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13letSimpleTestyyxnYalFTQ0_"(ptr swiftasync %0)
// CHECK: entryresume.0:
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[SIMPLE_TEST_METADATA_2:[0-9]+]], metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)),
// CHECK:   musttail call swifttailcc void
// CHECK-NEXT: ret void
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13letSimpleTestyyxnYalFTY1_"(ptr swiftasync %0)
// CHECK: entryresume.1:
// CHECK:     call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[SIMPLE_TEST_METADATA_3:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:     call void @llvm.dbg.value(metadata ptr undef, metadata ![[SIMPLE_TEST_METADATA_3]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:   musttail call swifttailcc void
// CHECK-NEXT: ret void

// RUN: %llvm-dwarfdump -c --name='$s3out13letSimpleTestyyxnYalF' %t/out.o | %FileCheck -check-prefix=DWARF1 %s
// DWARF1:  DW_AT_linkage_name	("$s3out13letSimpleTestyyxnYalF")
// DWARF1:  DW_TAG_formal_parameter
// DWARF1-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:DW_OP_.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF1-NEXT:  DW_AT_name ("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out13letSimpleTestyyxnYalFTQ0_' %t/out.o | %FileCheck -check-prefix=DWARF2 %s
// DWARF2:  DW_AT_linkage_name	("$s3out13letSimpleTestyyxnYalFTQ0_")
// DWARF2:  DW_AT_name	("letSimpleTest")
// DWARF2:  DW_TAG_formal_parameter
// DWARF2-NEXT:  DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:DW_OP_.*]]), DW_OP_deref, DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF2-NEXT:  DW_AT_name	("msg")
//

// RUN: %llvm-dwarfdump -c --name='$s3out13letSimpleTestyyxnYalFTY1_' %t/out.o | %FileCheck -check-prefix=DWARF3 %s
// DWARF3: DW_AT_linkage_name	("$s3out13letSimpleTestyyxnYalFTY1_")
// DWARF3: DW_AT_name	("letSimpleTest")
// DWARF3: DW_TAG_formal_parameter
// DWARF3: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF3-NEXT:            [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF3-NEXT:            DW_AT_name	("msg")
public func letSimpleTest<T>(_ msg: __owned T) async {
    await forceSplit()
    use(consume msg)
}

// CHECK-LABEL: define swifttailcc void @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalF"(ptr swiftasync %0, ptr %1, ptr noalias %2, ptr %T)
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref))
// CHECK:   musttail call swifttailcc void @"$s27move_function_dbginfo_async10forceSplityyYaF"(ptr swiftasync %{{[0-9]+}})
// CHECK-NEXT:   ret void
// CHECK-NEXT: }
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalFTQ0_"(ptr swiftasync %0)
// CHECK: entryresume.0:
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref))
// CHECK: musttail call swifttailcc void @swift_task_switch(ptr swiftasync %{{[0-9]+}}, ptr @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalFTY1_", i64 0, i64 0)
// CHECK-NEXT: ret void
// CHECK-NEXT: }
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalFTY1_"(ptr swiftasync %0)
// CHECK: entryresume.1:
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:   call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:   musttail call swifttailcc void @"$s27move_function_dbginfo_async10forceSplityyYaF"(ptr swiftasync
// CHECK-NEXT: ret void
// CHECK-NEXT: }
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalFTQ2_"(ptr swiftasync %0)
// CHECK: entryresume.2:

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async13varSimpleTestyyxz_xtYalFTY3_"(ptr swiftasync %0)
// CHECK: entryresume.3:
// CHECK:    call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:   call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK: musttail call swifttailcc void @"$s27move_function_dbginfo_async10forceSplityyYaF"(
// CHECK-NEXT:   ret void
// CHECK-NEXT: }
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalF' %t/out.o | %FileCheck -check-prefix=DWARF4 %s
// DWARF4: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalF")
// DWARF4: DW_AT_name	("varSimpleTest")
// DWARF4: DW_TAG_formal_parameter
// DWARF4-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF4-NEXT: DW_AT_name ("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTQ0_' %t/out.o | %FileCheck -check-prefix=DWARF5 %s
// DWARF5: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTQ0_")
// DWARF5: DW_AT_name	("varSimpleTest")
// DWARF5: DW_TAG_formal_parameter
// DWARF5-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF5-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTY1_' %t/out.o | %FileCheck -check-prefix=DWARF6 %s
// DWARF6: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTY1_")
// DWARF6: DW_AT_name	("varSimpleTest")
// DWARF6: DW_TAG_formal_parameter
// DWARF6-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF6-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF6-NEXT: DW_AT_name	("msg")
//
// We were just moved and are not reinit yet. This is caused by us hopping twice
// when we return from an async function. Once for the async function and then
// for the hop to executor.
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTQ2_' %t/out.o | %FileCheck -check-prefix=DWARF7 %s
// DWARF7: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTQ2_")
// DWARF7: DW_AT_name	("varSimpleTest")
// DWARF7: DW_TAG_formal_parameter
// DWARF7-NEXT: DW_AT_name ("msg")
//
// We reinitialize our value in this funclet and then consume it and then
// reinitialize it again. So we have two different live ranges. Sadly, we don't
// validate that the first live range doesn't start at the beginning of the
// function. But we have lldb tests to validate that.
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTY3_' %t/out.o | %FileCheck -check-prefix=DWARF8 %s
// DWARF8: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTY3_")
// DWARF8: DW_AT_name	("varSimpleTest")
// DWARF8: DW_TAG_formal_parameter
// DWARF8: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF8-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}):
// DWARF8-SAME:        DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref
// DWARF8-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}):
// DWARF8-SAME:        DW_OP_entry_value([[ASYNC_REG]]), DW_OP_plus_uconst 0x[[MSG_LOC]], DW_OP_plus_uconst 0x8, DW_OP_deref
// DWARF8-NEXT: DW_AT_name	("msg")
//
// We did not consume the value again here, so we just get a normal entry value for
// the entire function.
//
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTQ4_' %t/out.o | %FileCheck -check-prefix=DWARF9 %s
// DWARF9: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTQ4_")
// DWARF9: DW_AT_name	("varSimpleTest")
// DWARF9: DW_TAG_formal_parameter
// DWARF9-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x[[MSG_LOC:[a-f0-9]+]], DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF9-NEXT: DW_AT_name	("msg")
// 
// RUN: %llvm-dwarfdump -c --name='$s3out13varSimpleTestyyxz_xtYalFTY5_' %t/out.o | %FileCheck -check-prefix=DWARF10 %s
// DWARF10: DW_AT_linkage_name	("$s3out13varSimpleTestyyxz_xtYalFTY5_")
// DWARF10: DW_AT_name	("varSimpleTest")
// DWARF10: DW_TAG_formal_parameter
// DWARF10-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref
// DWARF10-NEXT: DW_AT_name	("msg")

// Change name to varSimpleTestArg
public func varSimpleTest<T>(_ msg: inout T, _ msg2: T) async {
    await forceSplit()
    use(consume msg)
    await forceSplit()
    msg = msg2
    let msg3 = consume msg
    let _ = msg3
    msg = msg2
    await forceSplit()
}

// We don't have an argument here, so we shouldn't have an llvm.dbg.addr in the
// initial function.
//
// CHECK-LABEL: define swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaF"(ptr swiftasync %0)
// CHECK-NOT: llvm.dbg.addr
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaFTY0_"(ptr swiftasync %0)
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8))
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaFTQ1_"(ptr swiftasync %0)
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8))

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaFTY2_"(ptr swiftasync %0)
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK: call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression()), !dbg ![[ADDR_LOC]]

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaFTQ3_"(ptr swiftasync %0)
// We should only see an llvm.dbg.value here.
// CHECK-NOT: llvm.dbg.addr
// CHECK: call void @llvm.dbg.value(metadata ptr undef,
// CHECK-NOT: llvm.dbg.addr
//
// We should see first a llvm.dbg.value to undef the value until we reinit. Then
// we should see a llvm.dbg.addr to reinit.
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async16varSimpleTestVaryyYaFTY4_"(ptr swiftasync %0)
// CHECK: call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA:[0-9]+]], metadata !DIExpression()), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8)), !dbg ![[ADDR_LOC]]

// We are not an argument, so no problem here.
//
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaF' %t/out.o | %FileCheck -check-prefix=DWARF11 %s
// DWARF11: DW_AT_linkage_name	("$s3out16varSimpleTestVaryyYaF")
//
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaFTY0_' %t/out.o | %FileCheck -check-prefix=DWARF12 %s
// DWARF12: DW_AT_linkage_name	("$s3out16varSimpleTestVaryyYaFTY0_")
//
// DWARF12:    DW_TAG_variable
// DWARF12-NEXT: DW_AT_location   (DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8)
// DWARF12-NEXT: DW_AT_name       ("k")
//
// MISSING-DWARF12:    DW_TAG_variable
// MISSING-DWARF12-NEXT: DW_AT_location
// MISSING-DWARF12-NEXT: DW_AT_name ("m")
//
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaFTQ1_' %t/out.o | %FileCheck -check-prefix=DWARF13 %s
// DWARF13: DW_AT_linkage_name	("$s3out16varSimpleTestVaryyYaFTQ1_")
//
// DWARF13:    DW_TAG_variable
// DWARF13-NEXT: DW_AT_location   (DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8)
// DWARF13-NEXT: DW_AT_name       ("k")
//
// MISSING-DWARF13:    DW_TAG_variable
// We don't pattern match the actual entry value of "m" since we don't guarantee
// it is an entry value since it isn't moved.
// MISSING-DWARF13-NEXT: DW_AT_location
// MISSING-DWARF13-NEXT: DW_AT_name ("m")
//
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaFTY2_' %t/out.o | %FileCheck -check-prefix=DWARF14 %s
// DWARF14: DW_AT_linkage_name	("$s3out16varSimpleTestVaryyYaFTY2_")
// DWARF14:    DW_TAG_variable
// DWARF14-NEXT: DW_AT_location   (0x{{[0-9a-f]+}}:
// DWARF14-NEXT:    [0x{{[0-9a-f]+}}, 0x{{[0-9a-f]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8)
// DWARF14-NEXT: DW_AT_name       ("k")
// DWARF14:    DW_TAG_variable
// DWARF14-NEXT: DW_AT_location
// DWARF14-NEXT: DW_AT_name ("m")
//
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaFTQ3_' %t/out.o | %FileCheck -check-prefix=DWARF15 %s
// DWARF15: DW_AT_linkage_name  ("$s3out16varSimpleTestVaryyYaFTQ3_")
// MISSING-DWARF15: DW_TAG_variable
// We don't pattern match the actual entry value of "m" since we don't guarantee
// it is an entry value since it isn't moved.
// MISSING-DWARF15-NEXT: DW_AT_location
// MISSING-DWARF15-NEXT: DW_AT_name  ("m")
// K is dead here.
// DWARF15: DW_TAG_variable
// DWARF15-NEXT:    DW_AT_name  ("k")
//
// We reinitialize k in 4.
// RUN: %llvm-dwarfdump -c --name='$s3out16varSimpleTestVaryyYaFTY4_' %t/out.o | %FileCheck -check-prefix=DWARF16 %s
// DWARF16: DW_AT_linkage_name  ("$s3out16varSimpleTestVaryyYaFTY4_")
// MISSING-DWARF16: DW_TAG_variable
// We don't pattern match the actual entry value of "m" since we don't guarantee
// it is an entry value since it isn't moved.
// MISSING-DWARF16-NEXT: DW_AT_location
// MISSING-DWARF16-NEXT: DW_AT_name  ("m")
// DWARF16: DW_TAG_variable
// DWARF16-NEXT: DW_AT_location  (0x{{[0-9a-f]+}}:
// DWARF16-NEXT: [0x{{[0-9a-f]+}}, 0x{{[0-9a-f]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8)
// DWARF16-NEXT: DW_AT_name ("k")
public func varSimpleTestVar() async {
    var k = Klass()
    k.doSomething()
    await forceSplit()
    let m = consume k
    m.doSomething()
    await forceSplit()
    k = Klass()
    k.doSomething()
    print("stop here")
}

////////////////////////////////////
// Conditional Control Flow Tests //
////////////////////////////////////

// CHECK-LABEL: define swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalF"(
// CHECK:  call void @llvm.dbg.addr(
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit1yyYaF"(
// CHECK-NEXT: ret void
// CHECK-NEXT: }

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTQ0_"(
// CHECK:  call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)),
// CHECK:  musttail call swifttailcc void @swift_task_switch(ptr swiftasync %{{[0-9]+}}, ptr @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTY1_", i64 0, i64 0)
// CHECK-NEXT: ret void
// CHECK-NEXT: }

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTY1_"(
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]*]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]*]]
// CHECK-NEXT: br label %[[BLOCK:[a-zA-Z\.0-9]*]],
//
// CHECK: [[BLOCK]]:
// CHECK:    br i1 %{{[0-9]}}, label %[[LHS_BLOCK:[a-zA-Z\.0-9]*]], label %[[RHS_BLOCK:[a-zA-Z\.0-9]*]],
//
// CHECK: [[LHS_BLOCK]]:
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit2yyYaF"(
// CHECK-NEXT:  ret void
//
// CHECK: [[RHS_BLOCK]]:
// CHECK-NOT: call void @llvm.dbg.value(
// CHECK-NOT: call void @llvm.dbg.addr(
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit3yyYaF"(
// CHECK-NEXT:  ret void
//
// We have two undef here due to the coroutine cloner functionally tail
// duplicating the true and the continuation. This causes us to have a
// debug_value undef from the merge point and one that was propagated from the
// _move.
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTQ2_"(
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA:[0-9]*]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]*]]
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit4yyYaF"(
// CHECK-NEXT:  ret void
//
// This is the false branch.
//
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTQ3_"(
// CHECK:  call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 8, DW_OP_deref))
// CHECK:   musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit4yyYaF"(
// CHECK-NEXT:   ret void,
// CHECK-NEXT: }
//
// This is the continuation block
// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20letArgCCFlowTrueTestyyxnYalFTQ4_"(
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata !{{.*}}, metadata !DIExpression(DW_OP_deref)),

// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalF' %t/out.o | %FileCheck -check-prefix=DWARF17 %s
// DWARF17: DW_TAG_subprogram
// DWARF17: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalF")
// DWARF17: DW_AT_name	("letArgCCFlowTrueTest")

// DWARF17: DW_TAG_formal_parameter
// DWARF17-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF17-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalFTQ0_' %t/out.o | %FileCheck -check-prefix=DWARF18 %s
// DWARF18: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalFTQ0_")
// DWARF18-NEXT: DW_AT_name	("letArgCCFlowTrueTest")
// DWARF18: DW_TAG_formal_parameter
// DWARF18-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF18-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalFTY1_' %t/out.o | %FileCheck -check-prefix=DWARF19 %s
// DWARF19: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalFTY1_")
// DWARF19-NEXT: DW_AT_name	("letArgCCFlowTrueTest")
// DWARF19: DW_TAG_formal_parameter
// DWARF19-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF19-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref
// DWARF19-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF19-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalFTQ2_' %t/out.o | %FileCheck -check-prefix=DWARF20 %s
// DWARF20: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalFTQ2_")
// DWARF20-NEXT: DW_AT_name	("letArgCCFlowTrueTest")
// DWARF20: DW_TAG_formal_parameter
// DWARF20-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalFTQ3_' %t/out.o | %FileCheck -check-prefix=DWARF21 %s
// DWARF21: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalFTQ3_")
// DWARF21-NEXT: DW_AT_name	("letArgCCFlowTrueTest")
// DWARF21: DW_TAG_formal_parameter
// DWARF21-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF21-NEXT:                        [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x8, DW_OP_deref)
// DWARF21-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20letArgCCFlowTrueTestyyxnYalFTQ4_' %t/out.o | %FileCheck -check-prefix=DWARF22 %s
// DWARF22: DW_AT_linkage_name	("$s3out20letArgCCFlowTrueTestyyxnYalFTQ4_")
// DWARF22-NEXT: DW_AT_name	("letArgCCFlowTrueTest")
// DWARF22: DW_TAG_formal_parameter
// DWARF22-NEXT: DW_AT_name	("msg")
public func letArgCCFlowTrueTest<T>(_ msg: __owned T) async {
    await forceSplit1()
    if trueValue {
        use(consume msg)
        await forceSplit2()
    } else {
        await forceSplit3()
    }
    await forceSplit4()
}

// We do an extra SIL check here to make sure we propagate merge points
// correctly.
// SIL-LABEL: sil @$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlF : $@convention(thin) @async <T where T : P> (@inout T) -> () {
// SIL: bb0([[ARG:%[0-9]+]] : $*T):
// SIL:   debug_value [moveable_value_debuginfo] [[ARG]]
// SIL:   [[FORCE_SPLIT_1:%[0-9]+]] = function_ref @$s27move_function_dbginfo_async11forceSplit1yyYaF : $@convention(thin) @async () -> ()
// SIL:   apply [[FORCE_SPLIT_1]]
// SIL-NEXT: debug_value [moveable_value_debuginfo] [[ARG]]
// SIL:   hop_to_executor
// SIL-NEXT: debug_value [moveable_value_debuginfo] [[ARG]]
// SIL:   cond_br {{%[0-9]+}}, bb1, bb2
//
// SIL: bb1:
// SIL:   debug_value [moveable_value_debuginfo] undef
// SIL:   [[FORCE_SPLIT_2:%[0-9]+]] = function_ref @$s27move_function_dbginfo_async11forceSplit2yyYaF : $@convention(thin) @async () -> ()
// SIL:   apply [[FORCE_SPLIT_2]]()
// SIL-NEXT:  debug_value [moveable_value_debuginfo] undef
// SIL:   hop_to_executor
// SIL-NEXT: debug_value [moveable_value_debuginfo] undef
// SIL:   br bb3
//
// SIL: bb2:
// SIL:   [[FORCE_SPLIT_3:%[0-9]+]] = function_ref @$s27move_function_dbginfo_async11forceSplit3yyYaF : $@convention(thin) @async () -> ()
// SIL:   apply [[FORCE_SPLIT_3]]
// SIL-NEXT: debug_value [moveable_value_debuginfo] [[ARG]]
// SIL:   debug_value [moveable_value_debuginfo] undef
// SIL:   hop_to_executor
// SIL-NEXT: debug_value [moveable_value_debuginfo] undef
//
// SIL: bb3:
// SIL-NEXT: debug_value [moveable_value_debuginfo] undef
// SIL: debug_value [moveable_value_debuginfo] [[ARG]]
// SIL: [[FORCE_SPLIT_4:%[0-9]+]] = function_ref @$s27move_function_dbginfo_async11forceSplit4yyYaF : $@convention(thin) @async () -> ()
// SIL: apply [[FORCE_SPLIT_4]]
// SIL-NEXT: debug_value [moveable_value_debuginfo] %0
// SIL: } // end sil function '$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlF'

// CHECK-LABEL: define swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlF"(
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)),
// CHECK: musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit1yyYaF"(

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ0_"(
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)),

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY1_"(
// CHECK:   call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:   br i1 %{{[0-9]+}}, label %[[LHS_BLOCK:[a-zA-Z0-9]+]], label %[[RHS_BLOCK:[a-zA-Z0-9]+]]

// CHECK: [[LHS_BLOCK]]:
// CHECK:    call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:    musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit2yyYaF"(

// CHECK: [[RHS_BLOCK]]:
// CHECK-NOT: call void @llvm.dbg.value
// CHECK:   musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit3yyYaF"(

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ2_"(
// CHECK-NOT: @llvm.dbg.addr
// CHECK:   call void @llvm.dbg.value(metadata ptr undef, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref)),
// CHECK-NOT: @llvm.dbg.addr
// CHECK:   musttail call swifttailcc void @swift_task_switch(ptr swiftasync %{{[0-9]+}}, ptr @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY3_", i64 0, i64 0)

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY3_"(
// CHECK-NOT: @llvm.dbg.addr
// CHECK:   call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:   call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit4yyYaF"(

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ4_"(
// CHECK-NOT: @llvm.dbg.value
// CHECK:  call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  musttail call swifttailcc void @swift_task_switch(ptr swiftasync %{{[0-9]+}}, ptr @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY5_",

// CHECK: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY5_"(
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA:[0-9]+]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC:[0-9]+]]
// CHECK:  call void @llvm.dbg.value(metadata ptr undef, metadata ![[METADATA]], metadata !DIExpression(DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata ![[METADATA]], metadata !DIExpression(DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)), !dbg ![[ADDR_LOC]]
// CHECK:  musttail call swifttailcc void @"$s27move_function_dbginfo_async11forceSplit4yyYaF"(

// CHECK-LABEL: define internal swifttailcc void @"$s27move_function_dbginfo_async20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ6_"(
// CHECK-NOT: @llvm.dbg.value(
// CHECK: call void @llvm.dbg.addr(metadata ptr %{{[0-9]+}}, metadata !{{[0-9]+}}, metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, 16, DW_OP_plus_uconst, 48, DW_OP_deref)),
// CHECK-NOT: @llvm.dbg.value(
// CHECK:  musttail call swifttailcc void %{{[0-9]+}}(ptr swiftasync
// CHECK-NEXT:  ret void,
// CHECK-NEXT: }

// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlF' %t/out.o | %FileCheck -check-prefix=DWARF24 %s
// DWARF24: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlF")
// DWARF24: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF24: DW_TAG_formal_parameter
// DWARF24-NEXT:                     DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF24-NEXT:                     DW_AT_name	("msg")
//

// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ0_' %t/out.o | %FileCheck -check-prefix=DWARF25 %s
// DWARF25: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ0_")
// DWARF25-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF25: DW_TAG_formal_parameter
// DWARF25-NEXT:   DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF25-NEXT:   DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY1_' %t/out.o | %FileCheck -check-prefix=DWARF26 %s
// DWARF26: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY1_")
// DWARF26-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF26: DW_TAG_formal_parameter
// DWARF26-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF26-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref
// DWARF26-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF26-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ2_' %t/out.o | %FileCheck -check-prefix=DWARF27 %s
// DWARF27: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ2_")
// DWARF-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF: DW_TAG_formal_parameter
// DWARF-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY3_' %t/out.o | %FileCheck -check-prefix=DWARF28 %s
// DWARF28: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY3_")
// DWARF28-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF28: DW_TAG_formal_parameter
// DWARF28-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF28-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF28-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ4_' %t/out.o | %FileCheck -check-prefix=DWARF29 %s
// DWARF29: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ4_")
// DWARF29-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF29: DW_TAG_formal_parameter
// DWARF29-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF29-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY5_' %t/out.o | %FileCheck -check-prefix=DWARF30 %s
// DWARF30: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTY5_")
// DWARF30-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF30: DW_TAG_formal_parameter
// DWARF30-NEXT: DW_AT_location	(0x{{[a-f0-9]+}}:
// DWARF30-NEXT:    [0x{{[a-f0-9]+}}, 0x{{[a-f0-9]+}}): DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF30-NEXT: DW_AT_name	("msg")
//
// RUN: %llvm-dwarfdump -c --name='$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ6_' %t/out.o | %FileCheck -check-prefix=DWARF31 %s
// DWARF31: DW_AT_linkage_name	("$s3out20varArgCCFlowTrueTestyyxzYaAA1PRzlFTQ6_")
// DWARF31-NEXT: DW_AT_name	("varArgCCFlowTrueTest")
// DWARF31: DW_TAG_formal_parameter
// DWARF31-NEXT: DW_AT_location	(DW_OP_entry_value([[ASYNC_REG:.*]]), DW_OP_deref, DW_OP_plus_uconst 0x10, DW_OP_plus_uconst 0x30, DW_OP_deref)
// DWARF31-NEXT: DW_AT_name	("msg")
public func varArgCCFlowTrueTest<T : P>(_ msg: inout T) async {
    await forceSplit1()
    if trueValue {
        use(consume msg)
        await forceSplit2()
    } else {
        await forceSplit3()
    }
    msg = T.value as! T
    await forceSplit4()
}
