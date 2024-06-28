// RUN: not %target-swift-frontend(mock-sdk: -sdk %/S/../ModuleInterface/Inputs/BadStdlib.sdk -module-cache-path %/t/module-cache -resource-dir %/S/../ModuleInterface/Inputs/BadStdlib.sdk) -c -primary-file %s -o %t.out -emit-module -emit-module-path %t.swiftmodule -module-name parseable_output_early -frontend-parseable-output 2>&1 | %FileCheck %s

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",

// CHECK:   "kind": "finished",
// CHECK-NEXT:   "name": "compile",
