// RUN: %target-swift-frontend -primary-file %s -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test -emit-sil -enforce-exclusivity=unchecked | %FileCheck %s

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test

import Swift

// CHECK-LABEL: sil @$s4test5test03strSiSS_tF : $@convention(thin) (@guaranteed String) -> Int

// CHECK: [[FN:%[0-9]+]] = function_ref @${{.*}}_findStringSwitchCase
// CHECK-NOT: _findStringSwitchCase
// CHECK: [[CMP:%[0-9]+]] = apply [[FN]]
// CHECK: [[CMP_RES:%[0-9]+]] = struct_extract [[CMP]]
// CHECK: [[CMP_RES_32:%[0-9]+]] = unchecked_trivial_bit_cast [[CMP_RES]] : $Builtin.Int64 to $Builtin.Int32
// CHECK: [[ZERO:%[0-9]+]] = integer_literal $Builtin.Int32, 0
// CHECK: [[ICMP:%[0-9]+]] = builtin "cmp_eq_Int32"([[CMP_RES_32]] : $Builtin.Int32, [[ZERO]] : $Builtin.Int32) : $Builtin.Int1
// CHECK: cond_br [[ICMP]], [[BB3:.*]], [[BB4:.*]] //

// CHECK: [[BB3]]:
// CHECK: [[ZERO:%.*]] = integer_literal $Builtin.{{Int64|Int32}}, 0
// CHECK: br [[DONE:.*]]([[ZERO]]

// CHECK: [[BB4]]:
// CHECK: [[ONE:%.*]] = integer_literal $Builtin.Int32, 1
// CHECK: [[ICMP2:%.*]] = builtin "cmp_eq_Int32"([[CMP_RES_32]] : $Builtin.Int32, [[ONE]] : $Builtin.Int32) : $Builtin.Int1
// CHECK: cond_br [[ICMP2]], [[BB5:.*]], [[BB6:.*]] //

// CHECK: [[BB5]]:
// CHECK: [[ONE_OUT:%.*]] = integer_literal $Builtin.{{Int64|Int32}}, 1
// CHECK: br [[DONE]]([[ONE_OUT]]

// CHECK: [[BB6]]:
// CHECK: [[TWO:%.*]] = integer_literal $Builtin.Int32, 2
// CHECK: [[ICMP3:%.*]] = builtin "cmp_eq_Int32"([[CMP_RES_32]] : $Builtin.Int32, [[TWO]] : $Builtin.Int32) : $Builtin.Int1
// CHECK: cond_br [[ICMP3]], [[BB7:.*]], [[BB8:.*]] //

// CHECK: [[BB7]]:
// CHECK: [[TWO_OUT:%.*]] = integer_literal $Builtin.{{Int64|Int32}}, 2
// CHECK: br [[DONE]]([[TWO_OUT]]

// CHECK: [[BB8]]:
// CHECK: [[NEGATIVE_ONE:%.*]] = integer_literal $Builtin.{{Int64|Int32}}, -1
// CHECK: br [[DONE]]([[NEGATIVE_ONE]]

// CHECK: [[DONE]]([[OUT_VAL:%.*]] : $Builtin.{{Int64|Int32}}):
// CHECK: [[OUT_INT:%.*]] = struct $Int ([[OUT_VAL]]
// CHECK: return [[OUT_INT]] : $Int

// CHECK-LABEL: end sil function '$s4test5test03strSiSS_tF'
public func test0(str: String) -> Int {
  switch str {
    case "0": return 0
    case "2": return 1
    case "3": return 2
    default: return -1
  }
}

// CHECK-LABEL: sil @$s4test5test11x5inputS2i_SStF : $@convention(thin) (Int, @guaranteed String) -> Int

// CHECK: [[FN:%[0-9]+]] = function_ref @${{.*}}_findStringSwitchCase
// CHECK-NOT: _findStringSwitchCase
// CHECK: [[CMP1:%[0-9]+]] = apply [[FN]]

// CHECK: [[CMP2:%[0-9]+]] = apply [[FN]]
// CHECK: struct_extract [[CMP2]]
// CHECK: [[ICMP2:%[0-9]+]] = builtin "cmp_eq_Int32"
// CHECK: cond_br [[ICMP2]]

// CHECK: struct_extract [[CMP1]]
// CHECK: [[ICMP1:%[0-9]+]] = builtin "cmp_eq_Int32"
// CHECK: cond_br [[ICMP1]]

// CHECK-LABEL: end sil function '$s4test5test11x5inputS2i_SStF'
public func test1(x: Int, input: String) -> Int {
  if x == 1 {
    if input == "a" { return 1 }
    else if input == "aa" { return 2 }
    return 3
  } else {
    if input == "x" { return x}
    else if input == "b" { return 2 }
    return 0
  }
}

// CHECK-LABEL: sil @$s4test5test21xSiSaySSG_tF : $@convention(thin) (@guaranteed Array<String>) -> Int
// CHECK: [[FN:%[0-9]+]] = function_ref @${{.*}}_findStringSwitchCase{{.*}}
// CHECK: [[CMP:%[0-9]+]] = apply [[FN]]
// CHECK: struct_extract [[CMP]]
// CHECK: [[ICMP:%[0-9]+]] = builtin "cmp_eq_Int32"
// CHECK: cond_br [[ICMP]]
// CHECK-LABEL: end sil function '$s4test5test21xSiSaySSG_tF'
public func test2(x: [String]) -> Int {
  for str in x {
    if str == "foo" { return 0 }
    else if str == "bar" { return 1 }
  }
  return 1
}

// Make sure that we only optimize 2+ comparisons.
// CHECK-LABEL: sil @$s4test5test31xSbSS_tF : $@convention(thin) (@guaranteed String) -> Bool
// CHECK-NOT: _findStringSwitchCase
// CHECK-LABEL: end sil function '$s4test5test31xSbSS_tF'
public func test3(x: String) -> Bool {
  return x == "str"
}

// TODO: we could eventually support unicode.
// Make sure that we don't optimize unicode (non-ascii) strings. We currently
// can't optimize these strings because C++ can't compare them correctly, so,
// in the following case, the string "ṩ" will be added to
// "_findStringSwitchCase" twice (because C++ thinks there are two different
// strings). But "_findStringSwitchCase" thinks both of those strings are the
// same, so it will always return the same index.
// CHECK-LABEL: sil @$s4test0A7UnicodeySbSSF : $@convention(thin) (@guaranteed String) -> Bool
// CHECK-NOT: _findStringSwitchCase
// CHECK-LABEL: end sil function '$s4test0A7UnicodeySbSSF'
public func testUnicode(_ subject: String) -> Bool {
  let a = "s\u{323}\u{307}"
  let b = "\u{1e69}"
  let comp1 = a == subject
  let comp2 = b == subject
  if comp1 != comp2 { fatalError() }
  return comp1 && comp2
}

// CHECK-OUTPUT: CHECK-ME
print("CHECK-ME")
// CHECK-OUTPUT-NEXT: 1
print(test1(x: 1, input: "a"))
// CHECK-OUTPUT-NEXT: 0
print(test1(x: 2, input: "a"))
// CHECK-OUTPUT-NEXT: 3
print(test1(x: 1, input: "x"))
// CHECK-OUTPUT-NEXT: 2
print(test1(x: 2, input: "b"))
// CHECK-OUTPUT-NEXT: 0
print(test1(x: 2, input: "c"))

// CHECK-OUTPUT-NEXT: 1
print(test2(x: ["a", "b", "c"]))
// CHECK-OUTPUT-NEXT: 0
print(test2(x: ["foo", "b", "c"]))
// CHECK-OUTPUT-NEXT: 0
print(test2(x: ["a", "foo", "c"]))
// CHECK-OUTPUT-NEXT: 0
print(test2(x: ["a", "b", "foo"]))
// CHECK-OUTPUT-NEXT: 0
print(test2(x: ["foo", "bar", "x"]))
// CHECK-OUTPUT-NEXT: 1
print(test2(x: ["bar", "foo", "x"]))

// CHECK-OUTPUT-NEXT: true
print(testUnicode("ṩ"))
// CHECK-OUTPUT-NEXT: true
print(testUnicode("s\u{323}\u{307}"))
// CHECK-OUTPUT-NEXT: true
print(testUnicode("\u{1e69}"))
// CHECK-OUTPUT-NEXT: false
print(testUnicode("X"))
