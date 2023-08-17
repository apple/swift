// RUN: %empty-directory(%t)
// RUN: cp %s %t/main.swift
// RUN: %target-build-swift -whole-module-optimization -module-name PlaygroundSupport -emit-module-path %t/PlaygroundSupport.swiftmodule -parse-as-library -c -o %t/PlaygroundSupport.o %S/Inputs/SilentPCMacroRuntime.swift %S/Inputs/PlaygroundsRuntime.swift
// RUN: %target-build-swift -Xfrontend -playground -Xfrontend -playground-high-performance -enable-experimental-feature PlaygroundExtendedCallbacks -o %t/main -I=%t %t/PlaygroundSupport.o %t/main.swift
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s
// RUN: %target-build-swift -Xfrontend -pc-macro -Xfrontend -playground -Xfrontend -playground-high-performance -enable-experimental-feature PlaygroundExtendedCallbacks -o %t/main2 -I=%t %t/PlaygroundSupport.o %t/main.swift
// RUN: %target-codesign %t/main2
// RUN: %target-run %t/main2 | %FileCheck %s
// REQUIRES: executable_test

import PlaygroundSupport

var a = true
if (a) {
  5
} else {
  7
}

for i in 0..<3 {
  i
}
// CHECK: [{{.*}}] __builtin_log_with_id_extended[a='true' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='5' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='0' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='1' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='2' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]

var b = true
for i in 0..<3 {
  i
  continue
}
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[b='true' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='0' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='1' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='2' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]

var c = true
for i in 0..<3 {
  i
  break
}
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[c='true' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
// CHECK-NEXT: [{{.*}}] __builtin_log_with_id_extended[='0' module: main{{.*}}. file: {{.*/main[\d]?.swift}}]
