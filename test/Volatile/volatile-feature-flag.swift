// RUN: %empty-directory(%t)
// RUN: not %target-swift-emit-ir %s -module-name main -parse-as-library 2>&1 | %FileCheck %s
// RUN: %target-swift-emit-ir %s -module-name main -parse-as-library -enable-experimental-feature Volatile

// REQUIRES: volatile

import _Volatile

// CHECK: importing _Volatile module requires '-enable-experimental-feature Volatile'
