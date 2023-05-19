// RUN: %target-swift-frontend -parse-as-library -O -emit-sil %s | %FileCheck %s

// REQUIRES: swift_in_compiler

// Check that values of internal and private global variables, which are provably assigned only 
// once, are propagated into their uses and enable further optimizations like constant
// propagation, simplifications, etc.

// Define some global variables.

public var VD = 3.1415
public var VI = 100

private var PVD = 3.1415
private var PVI = 100
private var PVIAssignTwice = 1
private var PVITakenAddress = 1


internal var IVD = 3.1415
internal var IVI = 100
internal var IVIAssignTwice = 1
internal var IVITakenAddress = 1

// Taking the address of a global should prevent from performing the propagation of its value.
@inline(never)
@_optimize(none)
public func takeInout<T>(_ x: inout T) {
}

// Compiler should detect that we assign a global here as well and prevent a global optimization.
public func assignSecondTime() {
  PVIAssignTwice = 2
  IVIAssignTwice = 2
}

// Having multiple assignments to a global should prevent from performing the propagation of its value.

// Loads from private global variables can be removed, 
// because they cannot be changed outside of this source file.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation013test_private_B11_var_doubleSdyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK: float_literal
// CHECK: struct
// CHECK: return
@inline(never)
public func test_private_global_var_double() -> Double {
  return PVD + 1.0 
}

// Loads from private global variables can be removed, 
// because they cannot be changed outside of this source file.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation013test_private_B8_var_intSiyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK: integer_literal
// CHECK: struct
// CHECK: return
@inline(never)
public func test_private_global_var_int() -> Int {
  return PVI + 1
}

// Loads from internal global variables can be removed if this is a WMO compilation, because
// they cannot be changed outside of this module.
// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation014test_internal_B11_var_doubleSdyF
// CHECK-WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO: float_literal
// CHECK-WMO: struct
// CHECK-WMO: return
@inline(never)
public func test_internal_global_var_double() -> Double {
  return IVD + 1.0 
}

// Loads from internal global variables can be removed if this is a WMO compilation, because
// they cannot be changed outside of this module.
// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation014test_internal_B8_var_intSiyF
// CHECK_WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO: integer_literal
// CHECK-WMO: struct
// CHECK_WMO: return
@inline(never)
public func test_internal_global_var_int() -> Int {
  return IVI + 1
}

// Loads from public global variables cannot be removed, because their values could be changed elsewhere.
// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation012test_public_B11_var_doubleSdyF
// CHECK-WMO: bb0:
// CHECK-WMO-NEXT: global_addr
// CHECK-WMO-NEXT: begin_access [read] [dynamic]
// CHECK-WMO-NEXT: struct_element_addr
// CHECK-WMO-NEXT: load
@inline(never)
public func test_public_global_var_double() -> Double {
  return VD + 1.0 
}


// Loads from public global variables cannot be removed, because their values could be changed elsewhere.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation012test_public_B8_var_intSiyF
// CHECK: bb0: 
// CHECK-NEXT: global_addr
// CHECK-NEXT: begin_access [read] [dynamic]
// CHECK-NEXT: struct_element_addr
// CHECK-NEXT: load
@inline(never)
public func test_public_global_var_int() -> Int {
  return VI + 1
}

// Values of globals cannot be propagated as there are multiple assignments to it.
// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation026test_internal_and_private_B25_var_with_two_assignmentsSiyF
// CHECK-WMO: bb0: 
// CHECK-WMO-DAG: global_addr
// CHECK-WMO-DAG: struct_element_addr
// CHECK-WMO-DAG: load
// CHECK-WMO-DAG: global_addr
// CHECK-WMO-DAG: struct_element_addr
// CHECK-WMO-DAG: load
// CHECK-WMO: return
@inline(never)
public func test_internal_and_private_global_var_with_two_assignments() -> Int {
  return IVIAssignTwice + PVIAssignTwice
}

// Values of globals cannot be propagated as their address was taken and
// therefore their value could have been changed elsewhere.
// CHECK-WMO-LABEL: sil @$s28globalopt_global_propagation05test_B13_take_addressSiyF
// CHECK-WMO: bb0:
// CHECK-WMO: global_addr
// CHECK-WMO: global_addr
// CHECK-WMO: struct_element_addr
// CHECK-WMO: load
// CHECK-WMO: struct_element_addr
// CHECK-WMO: load
// CHECK-WMO: return
public func test_global_take_address() -> Int {
  takeInout(&PVITakenAddress)
  takeInout(&IVITakenAddress)
  return IVITakenAddress + PVITakenAddress
}

struct IntWrapper1 {
  let val: Int
}

struct IntWrapper2 {
  let val: IntWrapper1
}

struct IntWrapper3 {
  let val: IntWrapper2
}

struct IntWrapper4 {
  let val:  IntWrapper2
  let val2: IntWrapper1
}

let IW3 = IntWrapper3(val: IntWrapper2(val: IntWrapper1(val: 10)))


let IW4 = IntWrapper4(val: IntWrapper2(val: IntWrapper1(val: 10)), val2: IntWrapper1(val: 100))

// Test accessing single Int wrapped into multiple structs, where each struct has only one field.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation34test_let_struct_wrapped_single_intSiyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK: integer_literal
// CHECK: struct
// CHECK: return

// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation34test_let_struct_wrapped_single_intSiyF
// CHECK-WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO: integer_literal
// CHECK-WMO: struct
// CHECK-WMO: return
@inline(never)
public func test_let_struct_wrapped_single_int() -> Int {
  return IW3.val.val.val + 1
}

// Test accessing multiple Int fields wrapped into multiple structs, where each struct may have
// multiple fields.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation37test_let_struct_wrapped_multiple_intsSiyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK: integer_literal
// CHECK: struct
// CHECK: return

// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation37test_let_struct_wrapped_multiple_intsSiyF
// CHECK-WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO: integer_literal
// CHECK-WMO: struct
// CHECK-WMO: return
@inline(never)
public func test_let_struct_wrapped_multiple_ints() -> Int {
  return IW4.val.val.val + IW4.val2.val + 1
}


let IT1 = ((10, 20), 30, 40)

let IT2 = (100, 200, 300)

// Test accessing multiple Int fields wrapped into multiple tuples, where each tuple may have
// multiple fields.
// CHECK-LABEL: sil [noinline] @$s28globalopt_global_propagation27test_let_tuple_wrapped_intsSiyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK: integer_literal
// CHECK: struct
// CHECK: return

// CHECK-WMO-LABEL: sil [noinline] @$s28globalopt_global_propagation27test_let_tuple_wrapped_intsSiyF
// CHECK-WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO: integer_literal
// CHECK-WMO: struct
// CHECK-WMO: return
@inline(never)
public func test_let_tuple_wrapped_ints() -> Int {
  return IT1.0.0 + IT2.1
}

class Foo {
  fileprivate static var x: Int = 0
}

// CHECK-LABEL: sil @$s28globalopt_global_propagation25test_optimize_init_staticSiyF
// CHECK: bb0:
// CHECK-NOT: global_addr
// CHECK-NEXT: integer_literal
// CHECK-NEXT: struct
// CHECK-NEXT: return

// CHECK-WMO-LABEL: sil @$s28globalopt_global_propagation25test_optimize_init_staticSiyF
// CHECK-WMO: bb0:
// CHECK-WMO-NOT: global_addr
// CHECK-WMO-NEXT: integer_literal
// CHECK-WMO-NEXT: struct
// CHECK-WMO-NEXT: return
public func test_optimize_init_static() -> Int {
  return Foo.x
}

