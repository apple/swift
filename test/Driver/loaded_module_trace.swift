// RUN: %empty-directory(%t)
// RUN: %target-build-swift -emit-module -module-name Module %S/Inputs/loaded_module_trace_empty.swift -o %t/Module.swiftmodule
// RUN: %target-build-swift -emit-module -module-name Module2 %S/Inputs/loaded_module_trace_imports_module.swift -o %t/Module2.swiftmodule -I %t
// RUN: %target-build-swift %s -emit-loaded-module-trace -o %t/loaded_module_trace -I %t
// RUN: %FileCheck -check-prefix=CHECK %s < %t/loaded_module_trace.trace.json
// RUN: %FileCheck -check-prefix=CHECK-CONFIRM-ONELINE %s < %t/loaded_module_trace.trace.json

// REQUIRES: swift_stable_abi

// CHECK: {
// CHECK: "version":2
// CHECK: "name":"loaded_module_trace"
// CHECK: "arch":"{{[^"]*}}"
// CHECK: "swiftmodules":[
// CHECK-DAG: "{{[^"]*\\[/\\]}}Module2.swiftmodule"
// CHECK-DAG: "{{[^"]*\\[/\\]}}Swift.swiftmodule{{(\\[/\\][^"]+[.]swiftmodule)?}}"
// CHECK-DAG: "{{[^"]*\\[/\\]}}SwiftOnoneSupport.swiftmodule{{(\\[/\\][^"]+[.]swiftmodule)?}}"
// CHECK-DAG: "{{[^"]*\\[/\\]}}Module.swiftmodule"
// CHECK: ]

// The checks are compressed into one line as the order of the objects varies
// between OSes due to change in filepaths.

// CHECK: "swiftmodulesDetailedInfo":[
// CHECK-DAG: {"name":"Module2","path":"{{[^"]*\\[/\\]}}Module2.swiftmodule","isImportedDirectly":true,"supportsLibraryEvolution":false}
// CHECK-DAG: {"name":"Swift","path":"{{[^"]*\\[/\\]}}Swift.swiftmodule{{(\\[/\\][^"]+[.]swiftmodule)?}}","isImportedDirectly":true,"supportsLibraryEvolution":true}
// CHECK-DAG: {"name":"SwiftOnoneSupport","path":"{{[^"]*\\[/\\]}}SwiftOnoneSupport.swiftmodule{{(\\[/\\][^"]+[.]swiftmodule)?}}","isImportedDirectly":true,"supportsLibraryEvolution":true}
// CHECK-DAG: {"name":"Module","path":"{{[^"]*\\[/\\]}}Module.swiftmodule","isImportedDirectly":false,"supportsLibraryEvolution":false}
// CHECK: ]
// CHECK: }

// Make sure it's all on a single line.
// CHECK-CONFIRM-ONELINE: {"name":{{.*}}]}

import Module2
