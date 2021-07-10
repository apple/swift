// RUN: %swiftc_driver_plain -target aarch64-unknwon-none-none -driver-print-jobs %s 2>&1 | %FileCheck %s

// CHECK: {{.*}}swift{{c|-frontend}}{{(.exe)?"?}} -frontend -c
// CHECK: {{.*}}clang{{(.exe)?"?}} -fuse-ld=gold
