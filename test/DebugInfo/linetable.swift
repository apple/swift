// RUN: %target-swift-frontend %s %use_no_opaque_pointers -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend %s -S -g -o - | %FileCheck %s --check-prefix ASM-CHECK

// REQUIRES: CPU=i386 || CPU=x86_64

import Swift
func markUsed<T>(_ t: T) {}

class MyClass
{
    var x : Int64
    init(input: Int64) { x = input }
    func do_something(_ input: Int64) -> Int64
    {
        return x * input
    }
}

func call_me(_ code: @escaping () -> Void)
{
    code ()
}

func main(_ x: Int64) -> Void
// CHECK-LABEL: define hidden {{.*}} void @"$s9linetable4main{{[_0-9a-zA-Z]*}}F"
{
    var my_class = MyClass(input: 10)
// Linetable continuity. Don't go into the closure expression.
// ASM-CHECK: .loc [[FILEID:[0-9]]] [[@LINE+1]] 5
    call_me (
// ASM-CHECK-NOT: .loc [[FILEID]] [[@LINE+1]] 5
// CHECK-LABEL: define {{.*}} @"$s9linetable4mainyys5Int64VFyycfU_Tf2in_n"({{.*}})
        {
            var result = my_class.do_something(x)
            markUsed(result)
// CHECK: call {{.*}} @swift_release {{.*}}
// CHECK: bitcast
// CHECK: llvm.lifetime.end
// CHECK: call {{.*}}llvm.lifetime.end{{.*}}, !dbg ![[CLOSURE_END:.*]]
// CHECK-NEXT: ret void, !dbg ![[CLOSURE_END]]
// CHECK: ![[CLOSURE_END]] = !DILocation(line: [[@LINE+1]],
        }
    )

// The swift_releases at the end should not jump to the point where
// that memory was retained/allocated and also not to line 0.
// ASM-CHECK-NOT: .loc [[FILEID]] 0 0
// ASM-CHECK: .loc [[FILEID]] [[@LINE+2]] 1
// ASM-CHECK: ret
}

// ASM-CHECK: {{^_?\$s9linetable4mainyys5Int64VFyycfU_Tf2in_n:}}
// ASM-CHECK-NOT: retq
// The end-of-prologue should have a valid location (0 is ok, too).
// ASM-CHECK: .loc [[FILEID]] {{0|34}} {{[0-9]+}} prologue_end

main(30)
