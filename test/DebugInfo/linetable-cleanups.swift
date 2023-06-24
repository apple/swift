// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

// TODO: check why this is failing on linux
// REQUIRES: OS=macosx

func markUsed<T>(_ t: T) {}

class Person {
    var name = "No Name"
    var age = 0
}

func main() {
    var person = Person()
    var b = [0,1,13]
    for element in b {
        markUsed("element = \(element)")
    }
    markUsed("Done with the for loop")
// CHECK: call {{.*}}void @"$s4main8markUsedyyxlF"
// CHECK: br label
// CHECK: {{[0-9]+}}:
// CHECK: call ptr @"$ss16IndexingIteratorVySaySiGGWOh"(ptr %{{.*}}), !dbg ![[LOOPHEADER_LOC:.*]]
// CHECK: call {{.*}}void @"$s4main8markUsedyyxlF"
// The cleanups should share the line number with the ret stmt.
// CHECK:  call ptr @"$sSaySiGWOh"(ptr %{{.*}}), !dbg ![[CLEANUPS:.*]]
// CHECK-NEXT:  llvm.lifetime.end
// CHECK-SAME:  !dbg ![[CLEANUPS]]
// CHECK-NEXT:  load
// CHECK-NEXT:  swift_release
// CHECK-NEXT:  llvm.lifetime.end
// CHECK-NEXT:  ret void, !dbg ![[CLEANUPS]]
// CHECK: ![[CLEANUPS]] = !DILocation(line: [[@LINE+1]], column: 1,
}
main()
