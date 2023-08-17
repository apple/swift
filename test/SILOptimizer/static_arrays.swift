// RUN: %target-swift-frontend  -primary-file %s -O -sil-verify-all -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test -emit-sil | %FileCheck %s
// RUN: %target-swift-frontend  -primary-file %s -O -sil-verify-all -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test -emit-ir | %FileCheck %s -check-prefix=CHECK-LLVM

// Also do an end-to-end test to check all components, including IRGen.
// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT
// REQUIRES: executable_test,swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: CPU=arm64 || CPU=x86_64
// REQUIRES: swift_in_compiler

// Check if the optimizer is able to convert array literals to statically initialized arrays.

// CHECK-LABEL: sil_global @$s4test4FStrV10globalFuncyS2icvpZ : $@callee_guaranteed (Int) -> Int = {
// CHECK:         %0 = function_ref @$s4test3fooyS2iF : $@convention(thin) (Int) -> Int
// CHECK-NEXT:    %initval = thin_to_thick_function %0
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of arrayLookup(_:)
// CHECK-NEXT:  sil_global private @{{.*}}arrayLookup{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 10
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 11
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 12
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of returnArray()
// CHECK-NEXT:  sil_global private @{{.*}}returnArray{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 20
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 21
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of returnStaticStringArray()
// CHECK-NEXT:  sil_global private @{{.*}}returnStaticStringArray{{.*}} = {
// CHECK-DAG:     string_literal utf8 "a"
// CHECK-DAG:     string_literal utf8 "b"
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of passArray()
// CHECK-NEXT:  sil_global private @{{.*}}passArray{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 27
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 28
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #1 of passArray()
// CHECK-NEXT:  sil_global private @{{.*}}passArray{{.*}} = {
// CHECK:         integer_literal $Builtin.Int{{[0-9]+}}, 29
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of storeArray()
// CHECK-NEXT:  sil_global private @{{.*}}storeArray{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 227
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 228
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of functionArray()
// CHECK-NEXT:  sil_global private @{{.*functionArray.*}} = {
// CHECK:         function_ref
// CHECK:         thin_to_thick_function
// CHECK:         convert_function
// CHECK:         function_ref
// CHECK:         thin_to_thick_function
// CHECK:         convert_function
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems]
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of returnDictionary()
// CHECK-NEXT:  sil_global private @{{.*}}returnDictionary{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 5
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 4
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 2
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 1
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 6
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 3
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems]
// CHECK-NEXT:  }

// CHECK-LABEL: outlined variable #0 of returnStringDictionary()
// CHECK-NEXT:  sil_global private @{{.*}}returnStringDictionary{{.*}} = {
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems]
// CHECK-NEXT:  }

// CHECK-LABEL: sil_global private @{{.*}}main{{.*}} = {
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 100
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 101
// CHECK-DAG:     integer_literal $Builtin.Int{{[0-9]+}}, 102
// CHECK:         object {{.*}} ({{[^,]*}}, [tail_elems] {{[^,]*}}, {{[^,]*}}, {{[^,]*}})
// CHECK-NEXT:  }

// CHECK-LABEL: sil {{.*}}@main
// CHECK:   global_value @{{.*}}main{{.*}}
// CHECK:   return
public let globalVariable = [ 100, 101, 102 ]

// CHECK-LABEL: sil [noinline] @$s4test11arrayLookupyS2iF
// CHECK:   global_value [bare] @$s4test11arrayLookupyS2iFTv_
// CHECK-NOT: retain
// CHECK-NOT: release
// CHECK:   } // end sil function '$s4test11arrayLookupyS2iF'

// CHECK-LLVM-LABEL: define {{.*}} @"$s4test11arrayLookupyS2iF"
// CHECK-LLVM-NOT:  call
// CHECK-LLVM:      [[E:%[0-9]+]] = getelementptr {{.*}} @"$s4test11arrayLookupyS2iFTv_"
// CHECK-LLVM-NEXT: [[L:%[0-9]+]] = load {{.*}} [[E]]
// CHECK-LLVM-NEXT: ret {{.*}} [[L]]
// CHECK-LLVM:   }
@inline(never)
public func arrayLookup(_ i: Int) -> Int {
  let lookupTable = [10, 11, 12]
  return lookupTable[i]
}

// CHECK-LABEL: sil {{.*}}returnArray{{.*}} : $@convention(thin) () -> @owned Array<Int> {
// CHECK:   global_value @{{.*}}returnArray{{.*}}
// CHECK:   return
@inline(never)
public func returnArray() -> [Int] {
  return [20, 21]
}

// CHECK-LABEL: sil {{.*}}returnStaticStringArray{{.*}} : $@convention(thin) () -> @owned Array<StaticString> {
// CHECK:   global_value @{{.*}}returnStaticStringArray{{.*}}
// CHECK:   return
@inline(never)
public func returnStaticStringArray() -> [StaticString] {
  return ["a", "b"]
}

public var gg: [Int]?

@inline(never)
public func receiveArray(_ a: [Int]) {
  gg = a
}

// CHECK-LABEL: sil {{.*}}passArray{{.*}} : $@convention(thin) () -> () {
// CHECK:   global_value @{{.*}}passArray{{.*}}
// CHECK:   global_value @{{.*}}passArray{{.*}}
// CHECK:   return
@inline(never)
public func passArray() {
  receiveArray([27, 28])
  receiveArray([29])
}

// CHECK-LABEL: sil {{.*}}storeArray{{.*}} : $@convention(thin) () -> () {
// CHECK:   global_value @{{.*}}storeArray{{.*}}
// CHECK:   return
@inline(never)
public func storeArray() {
  gg = [227, 228]
}

struct Empty { }

// CHECK-LABEL: sil {{.*}}arrayWithEmptyElements{{.*}} : $@convention(thin) () -> @owned Array<Empty> {
func arrayWithEmptyElements() -> [Empty] {
  // CHECK:    global_value @{{.*}}arrayWithEmptyElements{{.*}}
  // CHECK:    return
  return [Empty()]
}

// CHECK-LABEL: sil hidden [noinline] @$s4test13arrayOfTuplesSaySi_SbtGyF :
// CHECK:         global_value @$s4test13arrayOfTuplesSaySi_SbtGyFTv_ :
// CHECK-NOT:     store
// CHECK-NOT:     apply
// CHECK:       } // end sil function '$s4test13arrayOfTuplesSaySi_SbtGyF'
@inline(never)
func arrayOfTuples() -> [(Int, Bool)] {
  return [(1, false), (2, true), (3, false)]
}

// CHECK-LABEL: sil {{.*}}returnDictionary{{.*}} : $@convention(thin) () -> @owned Dictionary<Int, Int> {
// CHECK:   global_value @{{.*}}returnDictionary{{.*}}
// CHECK:   return
@inline(never)
public func returnDictionary() -> [Int:Int] {
  return [1:2, 3:4, 5:6]
}

// CHECK-LABEL: sil {{.*}}returnStringDictionary{{.*}} : $@convention(thin) () -> @owned Dictionary<String, String> {
// CHECK:   global_value @{{.*}}returnStringDictionary{{.*}}
// CHECK:   return
@inline(never)
public func returnStringDictionary() -> [String:String] {
  return ["1":"2", "3":"4", "5":"6"]
}

func foo(_ i: Int) -> Int { return i }

// CHECK-LABEL: sil {{.*functionArray.*}} : $@convention(thin) () -> @owned Array<(Int) -> Int> {
// CHECK:   global_value @{{.*functionArray.*}}
// CHECK: } // end sil function '{{.*functionArray.*}}'
@inline(never)
func functionArray() -> [(Int) -> Int] {
  func bar(_ i: Int) -> Int { return i + 1 }
  return [foo, bar, { $0 + 10 }]
}

var g1 = 1
var g2 = 2

// CHECK-LABEL: sil {{.*arrayOfGlobalPointers.*}} : $@convention(thin) () -> @owned Array<UnsafePointer<Int>> {
// CHECK:         global_value @{{.*arrayOfGlobalPointers.*}}
// CHECK:       } // end sil function '{{.*arrayOfGlobalPointers.*}}'
@inline(never)
public func arrayOfGlobalPointers() -> [UnsafePointer<Int>] {
  return [UnsafePointer(&g1), UnsafePointer(&g2)]
}

public struct FStr {
  // Not an array, but also tested here.
  public static var globalFunc = foo
}

@inline(never)
func testit() {
  // CHECK-OUTPUT:      [100, 101, 102]
  print(globalVariable)
  // CHECK-OUTPUT-NEXT: 11
  print(arrayLookup(1))
  // CHECK-OUTPUT-NEXT: [20, 21]
  print(returnArray())
  // CHECK-OUTPUT-NEXT: ["a", "b"]
  print(returnStaticStringArray())
  passArray()
  // CHECK-OUTPUT-NEXT: [29]
  print(gg!)
  storeArray()
  // CHECK-OUTPUT-NEXT: [227, 228]
  print(gg!)
  // CHECK-OUTPUT-NEXT: 311
  print(functionArray()[0](100) + functionArray()[1](100) + functionArray()[2](100))
  // CHECK-OUTPUT-NEXT: 27
  print(FStr.globalFunc(27))
  
  let tuples = arrayOfTuples()
  // CHECK-OUTPUT-NEXT: tuples [(1, false), (2, true), (3, false)]
  print("tuples \(tuples)")
  
  let dict = returnDictionary()
  // CHECK-OUTPUT-NEXT: dict 3: 2, 4, 6
  print("dict \(dict.count): \(dict[1]!), \(dict[3]!), \(dict[5]!)")
  
  let sdict = returnStringDictionary()
  // CHECK-OUTPUT-NEXT: sdict 3: 2, 4, 6
  print("sdict \(sdict.count): \(sdict["1"]!), \(sdict["3"]!), \(sdict["5"]!)")

  // CHECK-OUTPUT-NEXT: globalpointers: [1, 2]
  print("globalpointers: \(arrayOfGlobalPointers().map { $0.pointee })")
}

testit()

public class SwiftClass {}

@inline(never)
func takeUnsafePointer(ptr : UnsafePointer<SwiftClass>, len: Int) {
  print(ptr, len)  // Use the arguments somehow so they don't get removed.
}

// This should be a single basic block, and the array should end up being stack
// allocated.
//
// CHECK-LABEL: sil [noinline] @{{.*passArrayOfClasses.*}} : $@convention(thin) (@guaranteed SwiftClass, @guaranteed SwiftClass, @guaranteed SwiftClass) -> () {
// CHECK:       bb0(%0 : $SwiftClass, %1 : $SwiftClass, %2 : $SwiftClass):
// CHECK-NOT:   bb1(
// CHECK:         alloc_ref{{(_dynamic)?}} {{.*}}[tail_elems $SwiftClass *
// CHECK-NOT:   bb1(
// CHECK:       } // end sil function '{{.*passArrayOfClasses.*}}'
@inline(never)
public func passArrayOfClasses(a: SwiftClass, b: SwiftClass, c: SwiftClass) {
  let arr = [a, b, c]
  takeUnsafePointer(ptr: arr, len: arr.count)
}

