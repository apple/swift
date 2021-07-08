// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -Xllvm -sil-print-debuginfo -emit-sil %s | %FileCheck %s

import macros

// CHECK-LABEL: // testBitwiseOperations()
func testBitwiseOperations() {
  // CHECK: %[[P0:.*]] = integer_literal $Builtin.Int64, -1, loc {{.*}}
  // CHECK: %{{.*}} = struct $UInt64 (%[[P0]] : $Builtin.Int64), loc {{.*}}
  _ = DISPATCH_TIME_FOREVER as CUnsignedLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $UInt64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P1:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P1]] : $Builtin.Int32), loc {{.*}}
  _ = BIT_SHIFT_1 as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P2:.*]] = integer_literal $Builtin.Int32, 4, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P2]] : $Builtin.Int32), loc {{.*}}
  _ = BIT_SHIFT_2 as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P3:.*]] = integer_literal $Builtin.Int64, 24, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P3]] : $Builtin.Int64), loc {{.*}}
  _ = BIT_SHIFT_3 as CLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $Int64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P4:.*]] = integer_literal $Builtin.Int32, 2, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P4]] : $Builtin.Int32), loc {{.*}}
  _ = BIT_SHIFT_4 as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P5:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P5]] : $Builtin.Int32), loc {{.*}}
  _ = RSHIFT_ONE as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P6:.*]] = integer_literal $Builtin.Int32, -2, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P6]] : $Builtin.Int32), loc {{.*}}
  _ = RSHIFT_NEG as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P7:.*]] = integer_literal $Builtin.Int64, -4294967296, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt64 (%[[P7]] : $Builtin.Int64), loc {{.*}}
  _ = XOR_HIGH as CUnsignedLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $UInt64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P8:.*]] = integer_literal $Builtin.Int32, 256, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P8]] : $Builtin.Int32), loc {{.*}}
  _ = ATTR_BOLD as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P9:.*]] = integer_literal $Builtin.Int32, 512, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P9]] : $Builtin.Int32), loc {{.*}}
  _ = ATTR_ITALIC as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P10:.*]] = integer_literal $Builtin.Int32, 1024, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P10]] : $Builtin.Int32), loc {{.*}}
  _ = ATTR_UNDERLINE as CInt
}

// CHECK-LABEL: // testIntegerArithmetic()
func testIntegerArithmetic() {

  // CHECK: %[[P0:.*]] = integer_literal $Builtin.Int32, 0, loc {{.*}}
  // CHECK: %{{.*}} = struct $Int32 (%[[P0]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_ZERO as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P1:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P1]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_ONE as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P2:.*]] = integer_literal $Builtin.Int32, 2, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P2]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_TWO as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P3:.*]] = integer_literal $Builtin.Int32, -2, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P3]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_MINUS_TWO as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P4:.*]] = integer_literal $Builtin.Int64, 169, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P4]] : $Builtin.Int64), loc {{.*}}
  _ = ADD_MIXED_WIDTH as CLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $Int64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P5:.*]] = integer_literal $Builtin.Int64, 142, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P5]] : $Builtin.Int64), loc {{.*}}
  _ = ADD_MIXED_SIGN as CLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $Int64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P6:.*]] = integer_literal $Builtin.Int32, -3, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P6]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_UNDERFLOW as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P7:.*]] = integer_literal $Builtin.Int32, 2, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P7]] : $Builtin.Int32), loc {{.*}}
  _ = ADD_OVERFLOW as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P8:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P8]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_ONE as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P9:.*]] = integer_literal $Builtin.Int32, 0, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P9]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_ZERO as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P10:.*]] = integer_literal $Builtin.Int32, -1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P10]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_MINUS_ONE as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P11:.*]] = integer_literal $Builtin.Int64, 42, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P11]] : $Builtin.Int64), loc {{.*}}
  _ = SUB_MIXED_WIDTH as CLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $Int64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P12:.*]] = integer_literal $Builtin.Int32, 51, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P12]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_MIXED_SIGN as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P13:.*]] = integer_literal $Builtin.Int32, -1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P13]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_UNDERFLOW as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P14:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $UInt32 (%[[P14]] : $Builtin.Int32), loc {{.*}}
  _ = SUB_OVERFLOW as CUnsignedInt
  // CHECK-NEXT: debug_value %{{.*}} : $UInt32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P15:.*]] = integer_literal $Builtin.Int32, 36, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P15]] : $Builtin.Int32), loc {{.*}}
  _ = MULT_POS as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P16:.*]] = integer_literal $Builtin.Int32, -12, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P16]] : $Builtin.Int32), loc {{.*}}
  _ = MULT_NEG as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P17:.*]] = integer_literal $Builtin.Int64, 8589934590, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P17]] : $Builtin.Int64), loc {{.*}}
  _ = MULT_MIXED_TYPES as CLongLong
  // CHECK-NEXT: debug_value %{{.*}} : $Int64, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P18:.*]] = integer_literal $Builtin.Int32, 128, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P18]] : $Builtin.Int32), loc {{.*}}
  _ = DIVIDE_INTEGRAL as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P19:.*]] = integer_literal $Builtin.Int32, 1, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int32 (%[[P19]] : $Builtin.Int32), loc {{.*}}
  _ = DIVIDE_NONINTEGRAL as CInt
  // CHECK-NEXT: debug_value %{{.*}} : $Int32, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %[[P20:.*]] = integer_literal $Builtin.Int64, 2147483648, loc {{.*}}
  // CHECK:      %{{.*}} = struct $Int64 (%[[P20]] : $Builtin.Int64), loc {{.*}}
  _ = DIVIDE_MIXED_TYPES as CLongLong
}

// CHECK-LABEL: // testIntegerComparisons()
func testIntegerComparisons() {

  // CHECK: %0 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %1 = struct $Bool (%0 : $Builtin.Int1), loc {{.*}}
  _ = EQUAL_FALSE
  // CHECK-NEXT: debug_value %1 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %3 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %4 = struct $Bool (%3 : $Builtin.Int1), loc {{.*}}
  _ = EQUAL_TRUE
  // CHECK-NEXT: debug_value %4 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %6 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %7 = struct $Bool (%6 : $Builtin.Int1), loc {{.*}}
  _ = EQUAL_TRUE_MIXED_TYPES
  // CHECK-NEXT: debug_value %7 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %9 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %10 = struct $Bool (%9 : $Builtin.Int1), loc {{.*}}
  _ = GT_FALSE
  // CHECK-NEXT: debug_value %10 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %12 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %13 = struct $Bool (%12 : $Builtin.Int1), loc {{.*}}
  _ = GT_TRUE
  // CHECK-NEXT: debug_value %13 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %15 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %16 = struct $Bool (%15 : $Builtin.Int1), loc {{.*}}
  _ = GTE_FALSE
  // CHECK-NEXT: debug_value %16 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %18 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %19 = struct $Bool (%18 : $Builtin.Int1), loc {{.*}}
  _ = GTE_TRUE
  // CHECK-NEXT: debug_value %19 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %21 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %22 = struct $Bool (%21 : $Builtin.Int1), loc {{.*}}
  _ = LT_FALSE
  // CHECK-NEXT: debug_value %22 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %24 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %25 = struct $Bool (%24 : $Builtin.Int1), loc {{.*}}
  _ = LT_TRUE
  // CHECK-NEXT: debug_value %25 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %27 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %28 = struct $Bool (%27 : $Builtin.Int1), loc {{.*}}
  _ = LTE_FALSE
  // CHECK-NEXT: debug_value %28 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %30 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %31 = struct $Bool (%30 : $Builtin.Int1), loc {{.*}}
  _ = LTE_TRUE
}

// CHECK-LABEL: // testLogicalComparisons()
func testLogicalComparisons() {

  // CHECK: %0 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %1 = struct $Bool (%0 : $Builtin.Int1), loc {{.*}}
  _ = L_AND_TRUE
  // CHECK-NEXT: debug_value %1 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %3 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %4 = struct $Bool (%3 : $Builtin.Int1), loc {{.*}}
  _ = L_AND_FALSE
  // CHECK-NEXT: debug_value %4 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %6 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %7 = struct $Bool (%6 : $Builtin.Int1), loc {{.*}}
  _ = L_AND_TRUE_B
  // CHECK-NEXT: debug_value %7 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %9 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %10 = struct $Bool (%9 : $Builtin.Int1), loc {{.*}}
  _ = L_AND_FALSE_B
  // CHECK-NEXT: debug_value %10 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %12 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %13 = struct $Bool (%12 : $Builtin.Int1), loc {{.*}}
  _ = L_OR_TRUE
  // CHECK-NEXT: debug_value %13 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %15 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %16 = struct $Bool (%15 : $Builtin.Int1), loc {{.*}}
  _ = L_OR_FALSE
  // CHECK-NEXT: debug_value %16 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %18 = integer_literal $Builtin.Int1, -1, loc {{.*}}
  // CHECK-NEXT: %19 = struct $Bool (%18 : $Builtin.Int1), loc {{.*}}
  _ = L_OR_TRUE_B
  // CHECK-NEXT: debug_value %19 : $Bool, let, name "_", discarded, loc {{.*}}
  // CHECK-NEXT: %21 = integer_literal $Builtin.Int1, 0, loc {{.*}}
  // CHECK-NEXT: %22 = struct $Bool (%21 : $Builtin.Int1), loc {{.*}}
  _ = L_OR_FALSE_B
}
