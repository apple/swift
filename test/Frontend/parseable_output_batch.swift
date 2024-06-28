// RUN: %empty-directory(%t)
// RUN: touch %t/test.swift
// RUN: echo 'func foo() {}' > %t/test.swift
// RUN: %target-swift-frontend -c -primary-file %s -primary-file %t/test.swift -o %t.out -o test.tmp.out -module-name parseable_output_batch -emit-module -emit-module-path %t.swiftmodule -module-name test -emit-module -emit-module-path test.tmp.swiftmodule -frontend-parseable-output 2>&1 | %FileCheck %s

// Despite only one frontend invocation, two compile jobs are "begin" and "finish", using quasi-PIDs, on per-primary and its outputs.

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",
// CHECK-NEXT:   "command": "{{.*[\\/]}}swift-frontend{{(\.exe)?}}{{.*}}-primary-file {{.*[\\/]}}parseable_output_batch.swift -primary-file {{.*[\\/]}}test.swift -o {{.*[\\/]}}parseable_output_batch.swift.tmp.out -o test.tmp.out -module-name parseable_output_batch -emit-module -emit-module-path {{.*[\\/]}}parseable_output_batch.swift.tmp.swiftmodule -module-name test -emit-module -emit-module-path test.tmp.swiftmodule -frontend-parseable-output",
// CHECK-NEXT:   "command_executable": "{{.*[\\/]}}swift{{(-frontend|c)?(\.exe)?}}",
// CHECK-NEXT:   "command_arguments": [
// CHECK:           "-primary-file",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift",
// CHECK-NEXT:       "-primary-file",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp\/test.swift",
// CHECK-NEXT:       "-o",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp.out",
// CHECK-NEXT:       "-o",
// CHECK-NEXT:       "test.tmp.out",
// CHECK-NEXT:       "-module-name",
// CHECK-NEXT:       "parseable_output_batch",
// CHECK-NEXT:       "-emit-module",
// CHECK-NEXT:       "-emit-module-path",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp.swiftmodule",
// CHECK-NEXT:       "-module-name",
// CHECK-NEXT:       "test",
// CHECK-NEXT:       "-emit-module",
// CHECK-NEXT:       "-emit-module-path",
// CHECK-NEXT:       "test.tmp.swiftmodule",
// CHECK-NEXT:       "-frontend-parseable-output"
// CHECK-NEXT:     ],
// CHECK-NEXT:  "inputs": [
// CHECK-NEXT:    "{{.*[\\/]}}parseable_output_batch.swift"
// CHECK-NEXT:  ],
// CHECK-NEXT:  "outputs": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "image",
// CHECK-NEXT:      "path": "{{.*[\\/]}}parseable_output_batch.swift.tmp.out"
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "swiftmodule",
// CHECK-NEXT:      "path": "{{.*[\\/]}}parseable_output_batch.swift.tmp.swiftmodule"
// CHECK-NEXT:    }
// CHECK-NEXT:  ],
// CHECK-NEXT:  "pid": -1000,
// CHECK-NEXT:  "process": {
// CHECK-NEXT:    "real_pid": [[REALPID:[0-9]*]]
// CHECK-NEXT:  }
// CHECK-NEXT:}

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT: {
// CHECK-NEXT:   "kind": "began",
// CHECK-NEXT:   "name": "compile",
// CHECK-NEXT:   "command": "{{.*[\\/]}}swift-frontend{{(\.exe)?}}{{.*}}-primary-file {{.*[\\/]}}parseable_output_batch.swift -primary-file {{.*[\\/]}}test.swift -o {{.*[\\/]}}parseable_output_batch.swift.tmp.out -o test.tmp.out -module-name parseable_output_batch -emit-module -emit-module-path {{.*[\\/]}}parseable_output_batch.swift.tmp.swiftmodule -module-name test -emit-module -emit-module-path test.tmp.swiftmodule -frontend-parseable-output",
// CHECK-NEXT:   "command_executable": "{{.*[\\/]}}swift{{(-frontend|c)?(\.exe)?}}",
// CHECK-NEXT:   "command_arguments": [
// CHECK:            "-primary-file",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift",
// CHECK-NEXT:       "-primary-file",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp\/test.swift",
// CHECK-NEXT:       "-o",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp.out",
// CHECK-NEXT:       "-o",
// CHECK-NEXT:       "test.tmp.out",
// CHECK-NEXT:       "-module-name",
// CHECK-NEXT:       "parseable_output_batch",
// CHECK-NEXT:       "-emit-module",
// CHECK-NEXT:       "-emit-module-path",
// CHECK-NEXT:       "{{.*[\\/]}}parseable_output_batch.swift.tmp.swiftmodule",
// CHECK-NEXT:       "-module-name",
// CHECK-NEXT:       "test",
// CHECK-NEXT:       "-emit-module",
// CHECK-NEXT:       "-emit-module-path",
// CHECK-NEXT:       "test.tmp.swiftmodule",
// CHECK-NEXT:       "-frontend-parseable-output"
// CHECK-NEXT:     ],
// CHECK-NEXT:  "inputs": [
// CHECK-NEXT:    "{{.*[\\/]}}test.swift"
// CHECK-NEXT:  ],
// CHECK-NEXT:  "outputs": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "image",
// CHECK-NEXT:      "path": "test.tmp.out"
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "type": "swiftmodule",
// CHECK-NEXT:      "path": "test.tmp.swiftmodule"
// CHECK-NEXT:    }
// CHECK-NEXT:  ],
// CHECK-NEXT:  "pid": -1001,
// CHECK-NEXT:  "process": {
// CHECK-NEXT:    "real_pid": [[REALPID]]
// CHECK-NEXT:  }
// CHECK-NEXT:}

// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT:{
// CHECK-NEXT:  "kind": "finished",
// CHECK-NEXT:  "name": "compile",
// CHECK-NEXT:  "pid": -1000,
// CHECK-NEXT:  "process": {
// CHECK-NEXT:    "real_pid": [[REALPID]]
// CHECK-NEXT:  },
// CHECK-NEXT:  "exit-status": 0
// CHECK-NEXT:}
// CHECK: {{[1-9][0-9]*}}
// CHECK-NEXT:{
// CHECK-NEXT:  "kind": "finished",
// CHECK-NEXT:  "name": "compile",
// CHECK-NEXT:  "pid": -1001,
// CHECK-NEXT:  "process": {
// CHECK-NEXT:    "real_pid": [[REALPID]]
// CHECK-NEXT:  },
// CHECK-NEXT:  "exit-status": 0
// CHECK-NEXT:}
