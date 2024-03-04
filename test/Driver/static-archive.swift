// UNSUPPORTED: linker_overridden

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 -emit-library %s -module-name ARCHIVER -static 2>&1 | %FileCheck -check-prefix CHECK-MACOS %s

// CHECK-MACOS: swift
// CHECK-MACOS: -o [[OBJECTFILE:.*]]

// CHECK-MACOS-NEXT: {{(bin/)?}}libtool{{"?}} -static
// CHECK-MACOS-DAG: [[OBJECTFILE]]
// CHECK-MACOS: -o {{[^ ]+}}

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-linux-gnu -emit-library %s -module-name ARCHIVER -static 2>&1 | %FileCheck -check-prefix CHECK-LINUX %s

// CHECK-LINUX: swift
// CHECK-LINUX: -o [[OBJECTFILE:.*]]

// CHECK-LINUX: {{(bin/)?(llvm-)?}}ar{{(.exe)?"?}} crs

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-windows-msvc -emit-library %s -module-name ARCHIVER -static 2>&1 | %FileCheck -check-prefix CHECK-WINDOWS %s

// CHECK-WINDOWS: swift
// CHECK-WINDOWS: -o [[OBJECTFILE:.*]]

// CHECK-WINDOWS-NEXT: link{{(.exe)?"?}} /lib
// CHECK-WINDOWS-DAG: [[OBJECTFILE]]
// CHECK-WINDOWS: /OUT:{{[^ ]+}}

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-windows-msvc -use-ld=lld-link -emit-library %s -module-name ARCHIVER -static 2>&1 | %FileCheck -check-prefix CHECK-WINDOWS-LLD %s

// CHECK-WINDOWS-LLD: swift
// CHECK-WINDOWS-LLD: -o [[OBJECTFILE:.*]]

// CHECK-WINDOWS-LLD-NEXT: lld-link{{(.exe)?"?}} /lib
// CHECK-WINDOWS-LLD-DAG: [[OBJECTFILE]]
// CHECK-WINDOWS-LLD: /OUT:{{[^ ]+}}

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 -emit-library %s -module-name ARCHIVER -static | %FileCheck -check-prefix INFERRED_NAME_DARWIN %s
// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-linux-gnu -emit-library %s -module-name ARCHIVER -static | %FileCheck -check-prefix INFERRED_NAME_LINUX %s
// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-windows-msvc -emit-library %s -module-name ARCHIVER -static | %FileCheck -check-prefix INFERRED_NAME_WINDOWS %s

// INFERRED_NAME_DARWIN: bin{{/|\\\\}}swift{{c?(\.EXE)?}}
// INFERRED_NAME_DARWIN: -module-name ARCHIVER
// INFERRED_NAME_DARWIN: libtool -static
// INFERRED_NAME_DARWIN:  -o libARCHIVER.a
// INFERRED_NAME_LINUX:   libARCHIVER.a
// INFERRED_NAME_WINDOWS: -lib
// INFERRED_NAME_WINDOWS: libARCHIVER.lib

// RUN: not %swiftc_driver -driver-print-jobs -module-name ARCHIVER  %s -emit-executable -static 2>&1 | %FileCheck -check-prefix ERROR %s

// ERROR: error: -static may not be used with -emit-executable
