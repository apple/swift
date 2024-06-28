// RUN: %target-swift-frontend -c -primary-file %s %S/Inputs/filelist-other.swift -o %t.out -module-name parseable_output -empty-abi-descriptor -emit-abi-descriptor-path %t.abi.json -emit-module -emit-module-path %t.swiftmodule -serialize-diagnostics -serialize-diagnostics-path %t.dia -frontend-parseable-output 2>&1 | %FileCheck %s
// Check without primary files (WMO):
// RUN: %target-swift-frontend -c %s %S/Inputs/filelist-other.swift -o %t.out -module-name parseable_output -empty-abi-descriptor -emit-abi-descriptor-path %t.abi.json -emit-module -emit-module-path %t.swiftmodule -serialize-diagnostics -serialize-diagnostics-path %t.dia -frontend-parseable-output 2>&1 | %FileCheck %s

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",
// CHECK-NEXT:   "command": "{{.*[\\/]}}swift-frontend{{(\.exe)?}}{{.*}} {{.*[\\/]}}parseable_output.swift {{.*[\\/]}}filelist-other.swift -o {{.*[\\/]}}parseable_output.swift.tmp.out -module-name parseable_output -empty-abi-descriptor -emit-abi-descriptor-path {{.*[\\/]}}parseable_output.swift.tmp.abi.json -emit-module -emit-module-path {{.*[\\/]}}parseable_output.swift.tmp.swiftmodule -serialize-diagnostics -serialize-diagnostics-path {{.*[\\/]}}parseable_output.swift.tmp.dia -frontend-parseable-output",
// CHECK-NEXT:   "command_executable": "{{.*[\\/]}}swift{{(-frontend|c)?(\.exe)?}}",
// CHECK-NEXT:   "command_arguments": [
// CHECK:          "{{.*[\\/]}}parseable_output.swift",
// CHECK:          "-o",
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output.swift.tmp.out",
// CHECK-NEXT:     "-module-name",
// CHECK-NEXT:     "parseable_output",
// CHECK-NEXT:     "-empty-abi-descriptor",
// CHECK-NEXT:     "-emit-abi-descriptor-path",
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output.swift.tmp.abi.json",
// CHECK-NEXT:     "-emit-module",
// CHECK-NEXT:     "-emit-module-path",
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output.swift.tmp.swiftmodule",
// CHECK-NEXT:     "-serialize-diagnostics",
// CHECK-NEXT:     "-serialize-diagnostics-path"
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output.swift.tmp.dia",
// CHECK-NEXT:     "-frontend-parseable-output"
// CHECK-NEXT:   ],
// CHECK-NEXT:   "inputs": [
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output.swift"
// CHECK:        "outputs": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "image",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output.swift.tmp.out"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "swiftmodule",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output.swift.tmp.swiftmodule"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "diagnostics",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output.swift.tmp.dia"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "abi-baseline-json",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output.swift.tmp.abi.json"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "pid": [[PID:[0-9]*]]
// CHECK-NEXT:     "process": {
// CHECK-NEXT:       "real_pid": [[PID]]
// CHECK-NEXT:     }
// CHECK-NEXT:   }

// CHECK-NEXT: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "finished",
// CHECK-NEXT:   "name": "compile",
// CHECK-NEXT:   "pid": [[PID]],
// CHECK-NEXT:   "process": {
// CHECK-NEXT:     "real_pid": [[PID]]
// CHECK-NEXT:   },
// CHECK-NEXT:   "exit-status": 0
// CHECK-NEXT: }
