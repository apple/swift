// RUN: %empty-directory(%t2)

// RUN: %target-interop-build-clangxx -c %S/Inputs/objc-class-with-non-trivial-cxx-record.mm -o %t2/objc-class-impl.o -fobjc-arc

// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -fignore-exceptions -Xlinker %t2/objc-class-impl.o) | %FileCheck %s
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -fignore-exceptions -O -Xlinker %t2/objc-class-impl.o) | %FileCheck %s

// RUN: %target-interop-build-clangxx -c %S/Inputs/objc-class-with-non-trivial-cxx-record.mm -o %t2/objc-class-impl-non-trivial.o -fobjc-arc -DS_NONTRIVIAL_DESTRUCTOR
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -fignore-exceptions -Xcc -DS_NONTRIVIAL_DESTRUCTOR -Xlinker %t2/objc-class-impl-non-trivial.o) | %FileCheck %s
//
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation
import CxxClassWithNSStringInit

func testS() {
    let copy: S
    do {
        let foo: NSString? = "super long string actually allocated"
        let bar: NSString? = "bar"
        let baz: NSString? = "baz"
        var s = S(A: foo, B: bar, C: baz)
        s.dump()
        copy = s
    }
    print("after scope")
    copy.dump()
    print("takeSFunc")
    takeSFunc(copy)
}

@inline(never)
func blackHole<T>(_ x: T) {

}

func testReferenceStructToClassWithNonTrivialLogDestructorIvar() {
    print("testReferenceStructToClassWithNonTrivialLogDestructorIvar")
    let m = ReferenceStructToClassWithNonTrivialLogDestructorIvar(x: ClassWithNonTrivialDestructorIvar())
    m.x.takesS(S(A: "hello world two", B: "bar", C: "baz"))
    blackHole(m)
}

testS()
testReferenceStructToClassWithNonTrivialLogDestructorIvar()

// CHECK: super long string actually allocated
// CHECK-NEXT: bar
// CHECK-NEXT: baz
// CHECK-NEXT: after scope
// CHECK-NEXT: super long string actually allocated
// CHECK-NEXT: bar
// CHECK-NEXT: baz
// CHECK-NEXT: takeSFunc
// CHECK-NEXT: super long string actually allocated
// CHECK-NEXT: bar
// CHECK-NEXT: baz
// CHECK-NEXT: testReferenceStructToClassWithNonTrivialLogDestructorIvar
// CHECK-NEXT: takesS!
// CHECK-NEXT: hello world two
// CHECK-NEXT: bar
// CHECK-NEXT: baz
// CHECK-NEXT: ~NonTrivialLogDestructor 21
