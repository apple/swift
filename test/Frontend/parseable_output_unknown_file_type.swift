// RUN: %target-swift-frontend -c -primary-file %s %S/Inputs/filelist-other.swift -o %t.out -module-name parseable_output -empty-abi-descriptor -emit-abi-descriptor-path %t.abi.weird_file_extension -emit-module -emit-module-path %t.swiftmodule -frontend-parseable-output 2>&1 | %FileCheck %s

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",
// CHECK:        "inputs": [
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_unknown_file_type.swift"
// CHECK:        "outputs": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "image",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output_unknown_file_type.swift.tmp.out"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "swiftmodule",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output_unknown_file_type.swift.tmp.swiftmodule"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "type": "unknown",
// CHECK-NEXT:         "path": "{{.*[\\/]}}parseable_output_unknown_file_type.swift.tmp.abi.weird_file_extension"
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
