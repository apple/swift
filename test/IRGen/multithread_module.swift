// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/Inputs/multithread_module/main.swift -emit-ir -o %t/main.ll %/s -o %t/mt_module.ll -num-threads 2 -O -g -module-name test
// RUN: %FileCheck --check-prefix=CHECK-MAINLL %s <%t/main.ll
// RUN: %FileCheck --check-prefix=CHECK-MODULELL %s <%t/mt_module.ll

// RUN: %target-swift-frontend -c %S/Inputs/multithread_module/main.swift -o %t/main.o %s -o %t/mt_module.o -num-threads 2 -O -g -module-name test
// RUN: %target-build-swift %t/main.o %t/mt_module.o -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib,swift_stdlib_no_asserts


// Test compilation of a module in multi-threaded compilation.
// The main purpose of the test is to check that the generated LLVM modules are not corrupt
// and that linking succeeds.

// CHECK: 28
// CHECK: 125
// CHECK: 42
// CHECK: 237

public func testit(_ x: Int) -> Int {
	return incrementit(x)
}

public class Base {
	func memberfunc(_ x: Int) -> Int {
		return x + 1
	}
}

public var g2 = 123

@inline(never)
func callmember(_ b: Base) -> Int {
	return b.memberfunc(g2)
}

@inline(never)
private func privateInc(_ x: Int) -> Int {
	return x + 3
}

func callPrivInc(_ x: Int) -> Int {
	return privateInc(x)
}

// Check if we use the correct linkage for a transparent function
public var transparentfuncptr = transparentfunc

protocol MyProto {
	func protofunc() -> Int
}

@inline(never)
func callproto(_ p: MyProto) {
	print(p.protofunc())
}

public func mutateBaseArray(_ arr: inout [Base], _ x: Base) {
  arr.append(x)
}


// Check the llvm IR files:

// Check if all specializations from stdlib functions are created in the same LLVM module.

// CHECK-MAINLL-DAG: define {{.*}} @"$ss{{(12_|22_Contiguous)}}ArrayBufferV20_consumeAndCreateNew14bufferIsUnique15minimumCapacity13growForAppendAByxGSb_SiSbtF4test8MyStructV_Tg5"

// Check if the DI filename is correct and not "<unknown>".

// CHECK-MAINLL: [[F:![0-9]+]] = !DIFile(filename: "{{.*}}IRGen/Inputs/multithread_module/main.swift", directory: "{{.*}}")
// CHECK-MAINLL: DICompileUnit(language: DW_LANG_Swift, file: [[F]],

// CHECK-MODULELL: [[F:![0-9]]] = !DIFile(filename: "{{.*}}IRGen/multithread_module.swift", directory: "{{.*}}")
// CHECK-MODULELL: DICompileUnit(language: DW_LANG_Swift, file: [[F]],
