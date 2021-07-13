// UNSUPPORTED: freebsd

// RUN: %target-swiftc_driver -driver-print-bindings -embed-bitcode %s 2>&1 | %FileCheck -check-prefix=CHECK-%target-object-format %s
// CHECK-macho: "swift-frontend", inputs: ["{{.*}}embed-bitcode.swift"], output: {llvm-bc: "[[BC:.*\.bc]]"}
// CHECK-macho: "swift-frontend", inputs: ["[[BC]]"], output: {object: "[[OBJECT:.*\.o]]"}
// CHECK-macho: "ld", inputs: ["[[OBJECT]]"], output: {image: "embed-bitcode"}
// CHECK-coff: "swiftc.{{exe}}", inputs: ["{{.*}}embed-bitcode.swift"], output: {llvm-bc: "[[BC:.*\.bc]]"}
// CHECK-coff: "swiftc.{{exe}}", inputs: ["[[BC]]"], output: {object: "[[OBJECT:.*\.o]]"}
// CHECK-coff: "clang.exe", inputs: ["[[OBJECT]]"], output: {image: "embed-bitcode"}

// CHECK-elf: "swift-frontend", inputs: ["{{.*}}embed-bitcode.swift"], output: {llvm-bc: "[[BC:.*\.bc]]"}
// CHECK-elf: "swift-frontend", inputs: ["[[BC]]"], output: {object: "[[OBJECT:.*\.o]]"}
// CHECK-elf: "swift-autolink-extract", inputs: ["[[OBJECT]]"], output: {autolink: "[[AUTOLINK:.*\.autolink]]"}
// CHECK-elf: "clang", inputs: ["[[OBJECT]]", "[[AUTOLINK]]"], output: {image: "main"}

// RUN: %target-swiftc_driver -embed-bitcode %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-FRONT -check-prefix=CHECK-FRONT-%target-object-format
// CHECK-FRONT: -frontend
// CHECK-FRONT: -emit-bc
// CHECK-FRONT: -frontend
// CHECK-FRONT: -c
// CHECK-FRONT: -embed-bitcode{{ }}
// CHECK-FRONT: -disable-llvm-optzns
// CHECK-FRONT-macho: ld{{"? }}
// CHECK-FRONT-macho: -bitcode_bundle

// RUN: %target-swiftc_driver -embed-bitcode-marker %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-MARKER -check-prefix=CHECK-MARKER-%target-object-format
// CHECK-MARKER: -frontend
// CHECK-MARKER: -c
// CHECK-MARKER: -embed-bitcode-marker
// CHECK-MARKER-NOT: -frontend
// CHECK-MARKER-macho: ld{{"? }}
// CHECK-MARKER-macho: -bitcode_bundle

// RUN: %target-swiftc_driver -embed-bitcode -Xcc -DDEBUG -Xllvm -fake-llvm-option -c -emit-module %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-MODULE
// CHECK-MODULE: -frontend
// CHECK-MODULE: -emit-bc
// CHECK-MODULE-DAG: -Xcc -DDEBUG
// CHECK-MODULE-DAG: -Xllvm -fake-llvm-option
// CHECK-MODULE-DAG: -emit-module-path
// CHECK-MODULE: -frontend
// CHECK-MODULE: -emit-module
// CHECK-MODULE: -frontend
// CHECK-MODULE: -c
// CHECK-MODULE-NOT: -Xcc
// CHECK-MODULE-NOT: -DDEBUG
// CHECK-MODULE-NOT: -fake-llvm-option
// CHECK-MODULE-NOT: -emit-module-path

// RUN: %target-swiftc_driver -embed-bitcode -whole-module-optimization %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-SINGLE
// CHECK-SINGLE: -frontend
// CHECK-SINGLE: -emit-bc
// CHECK-SINGLE: -frontend
// CHECK-SINGLE: -c
// CHECK-SINGLE: -embed-bitcode
// CHECK-SINGLE: -disable-llvm-optzns

// RUN: %target-swiftc_driver -embed-bitcode -whole-module-optimization -O %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-SINGLE-OPT
// CHECK-SINGLE-OPT: -frontend
// CHECK-SINGLE-OPT-SAME: -emit-bc
// CHECK-SINGLE-OPT-SAME: -O{{[" ]}}
// CHECK-SINGLE-OPT-NEXT: -frontend
// CHECK-SINGLE-OPT-SAME: -c
// CHECK-SINGLE-OPT-SAME: -embed-bitcode
// CHECK-SINGLE-OPT-SAME: -O{{[" ]}}
// CHECK-SINGLE-OPT-SAME: -disable-llvm-optzns

// RUN: %target-swiftc_driver -embed-bitcode -whole-module-optimization -Osize %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-SINGLE-OPT-SIZE
// CHECK-SINGLE-OPT-SIZE: -frontend
// CHECK-SINGLE-OPT-SIZE-SAME: -emit-bc
// CHECK-SINGLE-OPT-SIZE-SAME: -Osize
// CHECK-SINGLE-OPT-SIZE-NEXT: -frontend
// CHECK-SINGLE-OPT-SIZE-SAME: -c
// CHECK-SINGLE-OPT-SIZE-SAME: -embed-bitcode
// CHECK-SINGLE-OPT-SIZE-SAME: -Osize
// CHECK-SINGLE-OPT-SIZE-SAME: -disable-llvm-optzns

// RUN: %target-swiftc_driver -embed-bitcode -target-cpu abc -whole-module-optimization %s 2>&1 -### | %FileCheck %s -check-prefix=CHECK-SINGLE-MISC
// CHECK-SINGLE-MISC: -frontend
// CHECK-SINGLE-MISC-SAME: -emit-bc
// CHECK-SINGLE-MISC-SAME: -target-cpu abc
// CHECK-SINGLE-MISC: -frontend
// CHECK-SINGLE-MISC-SAME: -c
// CHECK-SINGLE-MISC-SAME: -embed-bitcode
// CHECK-SINGLE-MISC-SAME: -target-cpu abc
// CHECK-SINGLE-MISC-SAME: -disable-llvm-optzns

// RUN: %target-swiftc_driver -embed-bitcode -c -parse-as-library -emit-module -whole-module-optimization %s -parse-stdlib -module-name Swift 2>&1 -### | %FileCheck %s -check-prefix=CHECK-LIB-WMO
// CHECK-LIB-WMO: -frontend
// CHECK-LIB-WMO: -emit-bc
// CHECK-LIB-WMO: -parse-stdlib
// CHECK-LIB-WMO: -frontend
// CHECK-LIB-WMO: -c
// CHECK-LIB-WMO: -embed-bitcode
// CHECK-LIB-WMO: -disable-llvm-optzns
// CHECK-LIB-WMO: -parse-stdlib

// RUN: %target-swiftc_driver -embed-bitcode -c -parse-as-library -emit-module %s %S/../Inputs/empty.swift -module-name ABC 2>&1 -### | %FileCheck %s -check-prefix=CHECK-LIB
// CHECK-LIB: swift{{(-frontend|c)?(\.exe)?"?}} -frontend
// CHECK-LIB: -emit-bc
// CHECK-LIB: -primary-file
// CHECK-LIB: swift{{(-frontend|c)?(\.exe)?"?}} -frontend
// CHECK-LIB: -emit-bc
// CHECK-LIB: -primary-file
// CHECK-LIB: swift{{(-frontend|c)?(\.exe)?"?}} -frontend
// CHECK-LIB: -c
// CHECK-LIB: -embed-bitcode
// CHECK-LIB: -disable-llvm-optzns
// CHECK-LIB: swift{{(-frontend|c)?(\.exe)?"?}} -frontend
// CHECK-LIB: -c
// CHECK-LIB: -embed-bitcode
// CHECK-LIB: -disable-llvm-optzns
// CHECK-LIB: swift{{(-frontend|c)?(\.exe)?"?}} -frontend
// CHECK-LIB: -emit-module
// CHECK-LIB-NOT: swift{{(-frontend|c)?(\.exe)?"?}} -frontend

// RUN: %target-swiftc_driver -embed-bitcode -emit-module %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-module-path a.swiftmodule %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-sib %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-sibgen %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-sil %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-silgen %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-ir %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-bc %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// RUN: %target-swiftc_driver -embed-bitcode -emit-assembly %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE
// WARN-EMBED-BITCODE: warning: ignoring -embed-bitcode since no object file is being generated
// WARN-EMBED-BITCODE-NOT: -embed-bitcode

// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-module %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-module-path a.swiftmodule %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-sib %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-sibgen %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-sil %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-silgen %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-ir %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-bc %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// RUN: %target-swiftc_driver -embed-bitcode-marker -emit-assembly %s 2>&1 -### | %FileCheck %s -check-prefix=WARN-EMBED-BITCODE-MARKER
// WARN-EMBED-BITCODE-MARKER: warning: ignoring -embed-bitcode-marker since no object file is being generated
// WARN-EMBED-BITCODE-MARKER-NOT: -embed-bitcode-marker
