// RUN: not %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck %s 2>&1 | %FileCheck %s --strict-whitespace

import macros

_ = INVALID_INTEGER_LITERAL_2

// Test that experimental diagnostics for the function like macro in the same header is suppressed.
// CHECK: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_INTEGER_LITERAL_2' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'FUNC_LIKE_MACRO' not imported: function like macros not supported
// CHECK-NOT: #define FUNC_LIKE_MACRO() 0
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'FUNC_LIKE_MACRO' not imported: function like macros not supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_1' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_2' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_3' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_4' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_5' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_6' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: invalid arithmetic operand; only valid integers supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'INVALID_ARITHMETIC_7' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: operator '%' not supported in macro arithmetic
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'CHAR_LITERAL' not imported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: only numeric and string macro literals supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'UNSUPPORTED_1' not imported: structure not supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'UNSUPPORTED_2' not imported: structure not supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'UNSUPPORTED_3' not imported: structure not supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'UNSUPPORTED_4' not imported: structure not supported
// CHECK-NOT: macros.h:{{[0-9]+}}:{{[0-9]+}}: error: macro 'UNSUPPORTED_5' not imported: structure not supported
