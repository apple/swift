// RUN: not %target-swift-frontend -c -primary-file %s -o %t.out -emit-module -emit-module-path %t.swiftmodule -frontend-parseable-output 2>&1 | %FileCheck %s
// Check without primary files (WMO):
// RUN: not %target-swift-frontend -c %s -o %t.out -emit-module -emit-module-path %t.swiftmodule -frontend-parseable-output 2>&1 | %FileCheck %s

func foo() {
    return 11;
}
// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",
// CHECK-NEXT:   "command": "{{.*[\\/]}}swift-frontend{{(\.exe)?}}{{.*}} {{.*[\\/]}}parseable_output_error.swift -o {{.*[\\/]}}parseable_output_error.swift.tmp.out  -emit-module -emit-module-path {{.*[\\/]}}parseable_output_error.swift.tmp.swiftmodule -frontend-parseable-output",
// CHECK-NEXT:   "command_executable": "{{.*[\\/]}}swift{{(-frontend|c)?(\.exe)?}}",
// CHECK-NEXT:   "command_arguments": [
// CHECK:     "{{.*[\\/]}}parseable_output_error.swift",
// CHECK-NEXT:     "-o",
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output_error.swift.tmp.out",
// CHECK-NEXT:     "-emit-module",
// CHECK-NEXT:     "-emit-module-path",
// CHECK-NEXT:     "{{.*[\\/]}}parseable_output_error.swift.tmp.swiftmodule",
// CHECK-NEXT:     "-frontend-parseable-output"
// CHECK-NEXT:   ],
// CHECK-NEXT:   "inputs": [
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_error.swift"
// CHECK-NEXT:     ],
// CHECK-NEXT:     "outputs": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "image",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output_error.swift.tmp.out"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "swiftmodule",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output_error.swift.tmp.swiftmodule"
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
// CHECK-NEXT:   "output": "{{.*[\\/]}}parseable_output_error.swift:6:12: error: unexpected non-void return value in void function{{.*}}return 11;{{.*[\\/]}}parseable_output_error.swift:6:12: note: did you mean to add a return type?{{.*}}return 11;
// CHECK-NEXT:   "process": {
// CHECK-NEXT:     "real_pid": [[PID]]
// CHECK-NEXT:   },
// CHECK-NEXT:   "exit-status": 1
// CHECK-NEXT: }
