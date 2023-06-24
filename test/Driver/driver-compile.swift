// RUN: %empty-directory(%t)

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s 2>&1 > %t.simple.txt
// RUN: %FileCheck %s < %t.simple.txt

// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s -sdk %S/../Inputs/clang-importer-sdk -Xfrontend -foo -Xfrontend -bar -Xllvm -baz -Xcc -garply -F /path/to/frameworks -Fsystem /path/to/systemframeworks -F /path/to/more/frameworks -I /path/to/headers -I path/to/more/headers -module-cache-path /tmp/modules -incremental 2>&1 > %t.complex.txt
// RUN: %FileCheck %s < %t.complex.txt
// RUN: %FileCheck -check-prefix COMPLEX %s < %t.complex.txt

// RUN: %swiftc_driver -driver-print-jobs -dump-ast -target x86_64-apple-macosx10.9 %s 2>&1 > %t.ast.txt
// RUN: %FileCheck %s < %t.ast.txt
// RUN: %FileCheck -check-prefix AST-STDOUT %s < %t.ast.txt

// RUN: %swiftc_driver -driver-print-jobs -dump-ast -target x86_64-apple-macosx10.9 %s -o output.ast > %t.ast.txt
// RUN: %FileCheck %s < %t.ast.txt
// RUN: %FileCheck -check-prefix AST-O %s < %t.ast.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-silgen -target x86_64-apple-macosx10.9 %s 2>&1 > %t.silgen.txt
// RUN: %FileCheck %s < %t.silgen.txt
// RUN: %FileCheck -check-prefix SILGEN %s < %t.silgen.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-sil -target x86_64-apple-macosx10.9 %s 2>&1 > %t.sil.txt
// RUN: %FileCheck %s < %t.sil.txt
// RUN: %FileCheck -check-prefix SIL %s < %t.sil.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-ir -target x86_64-apple-macosx10.9 %s 2>&1 > %t.ir.txt
// RUN: %FileCheck %s < %t.ir.txt
// RUN: %FileCheck -check-prefix IR %s < %t.ir.txt

// RUN: %swiftc_driver -driver-print-jobs -emit-bc -target x86_64-apple-macosx10.9 %s 2>&1 > %t.bc.txt
// RUN: %FileCheck %s < %t.bc.txt
// RUN: %FileCheck -check-prefix BC %s < %t.bc.txt

// RUN: %swiftc_driver -driver-print-jobs -S -target x86_64-apple-macosx10.9 %s 2>&1 > %t.s.txt
// RUN: %FileCheck %s < %t.s.txt
// RUN: %FileCheck -check-prefix ASM %s < %t.s.txt

// RUN: %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s 2>&1 > %t.c.txt
// RUN: %FileCheck %s < %t.c.txt
// RUN: %FileCheck -check-prefix OBJ %s < %t.c.txt

// RUN: not %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s %s 2>&1 | %FileCheck -check-prefix DUPLICATE-NAME %s
// RUN: cp %s %t
// RUN: not %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s %t/driver-compile.swift 2>&1 | %FileCheck -check-prefix DUPLICATE-NAME %s

// RUN: %swiftc_driver -driver-print-jobs -c -target x86_64-apple-macosx10.9 %s %S/../Inputs/empty.swift -module-name main -driver-filelist-threshold=0 2>&1 | %FileCheck -check-prefix=FILELIST %s

// RUN: %empty-directory(%t/DISTINCTIVE-PATH/usr/bin/)
// RUN: %empty-directory(%t/DISTINCTIVE-PATH/usr/lib/)
// RUN: %hardlink-or-copy(from: %swift_frontend_plain, to: %t/DISTINCTIVE-PATH/usr/bin/swiftc)
// RUN: ln -s "swiftc" %t/DISTINCTIVE-PATH/usr/bin/swift-update
// RUN: %host-library-env %t/DISTINCTIVE-PATH/usr/bin/swiftc -driver-print-jobs -c -update-code -target x86_64-apple-macosx10.9 %s 2>&1 > %t.upd.txt
// RUN: %FileCheck -check-prefix UPDATE-CODE %s < %t.upd.txt
// Clean up the test executable because hard links are expensive.
// RUN: rm -rf %t/DISTINCTIVE-PATH/usr/bin/swiftc

// RUN: %swiftc_driver -driver-print-jobs -whole-module-optimization -incremental %s 2>&1 > %t.wmo-inc.txt
// RUN: %FileCheck %s < %t.wmo-inc.txt
// RUN: %FileCheck -check-prefix NO-REFERENCE-DEPENDENCIES %s < %t.wmo-inc.txt

// RUN: %swiftc_driver -driver-print-jobs -embed-bitcode -incremental %s 2>&1 > %t.embed-inc.txt
// RUN: %FileCheck %s < %t.embed-inc.txt
// RUN: %FileCheck -check-prefix NO-REFERENCE-DEPENDENCIES %s < %t.embed-inc.txt

// REQUIRES: CODEGENERATOR=X86

// swift-frontend cannot be copied to another location with bootstrapping because
// it will not find the libswiftCore library with its relative RPATH.
// UNSUPPORTED: swift_in_compiler

// CHECK: bin{{/|\\\\}}swift
// CHECK: Driver{{/|\\\\}}driver-compile.swift
// CHECK: -o

// COMPLEX: bin{{/|\\\\}}swift
// COMPLEX: -c
// COMPLEX: Driver{{/|\\\\}}driver-compile.swift
// COMPLEX-DAG: -sdk
// COMPLEX-DAG: -foo -bar
// COMPLEX-DAG: -Xllvm -baz
// COMPLEX-DAG: -Xcc -garply
// COMPLEX-DAG: -F /path/to/frameworks -Fsystem /path/to/systemframeworks -F /path/to/more/frameworks
// COMPLEX-DAG: -I /path/to/headers -I path/to/more/headers
// COMPLEX-DAG: -module-cache-path /tmp/modules
// COMPLEX-DAG: -emit-reference-dependencies-path {{(.*(/|\\))?driver-compile[^ /]*}}.swiftdeps
// COMPLEX: -o {{.+}}.o


// AST-STDOUT: bin{{/|\\\\}}swift
// AST-STDOUT: -dump-ast
// AST-STDOUT: -o -

// AST-O: bin{{/|\\\\}}swift
// AST-O: -dump-ast
// AST-O: -o output.ast

// SILGEN: bin{{/|\\\\}}swift
// SILGEN: -emit-silgen
// SILGEN: -o -

// SIL: bin{{/|\\\\}}swift
// SIL: -emit-sil{{ }}
// SIL: -o -

// IR: bin{{/|\\\\}}swift
// IR: -emit-ir
// IR: -o -

// BC: bin{{/|\\\\}}swift
// BC: -emit-bc
// BC: -o {{[^-]}}

// ASM: bin{{/|\\\\}}swift
// ASM: -S{{ }}
// ASM: -o -

// OBJ: bin{{/|\\\\}}swift
// OBJ: -c{{ }}
// OBJ: -o {{[^-]}}

// DUPLICATE-NAME: error: filename "driver-compile.swift" used twice: '{{.*}}test{{[/\\]}}Driver{{[/\\]}}driver-compile.swift' and '{{.*}}driver-compile.swift'
// DUPLICATE-NAME: note: filenames are used to distinguish private declarations with the same name

// FILELIST: bin{{/|\\\\}}swift
// FILELIST: -filelist [[SOURCES:(["][^"]+sources[^"]+["]|[^ ]+sources[^ ]+)]]
// FILELIST: -primary-filelist  {{(["][^"]+primaryInputs[^"]+["]|[^ ]+primaryInputs[^ ]+)}}
// FILELIST: -supplementary-output-file-map {{(["][^"]+supplementaryOutputs[^"]+["]|[^ ]+supplementaryOutputs[^ ]+)}}
// FILELIST: -output-filelist {{[^-]}}
// FILELIST-NEXT: bin{{/|\\\\}}swift
// FILELIST: -filelist [[SOURCES]]
// FILELIST: -primary-filelist  {{(["][^"]+primaryInputs[^"]+["]|[^ ]+primaryInputs[^ ]+)}}
// FILELIST: -supplementary-output-file-map {{(["][^"]+supplementaryOutputs[^"]+["]|[^ ]+supplementaryOutputs[^ ]+)}}
// FILELIST: -output-filelist {{[^-]}}

// UPDATE-CODE: {{DISTINCTIVE-PATH|distinctive-path}}{{/|\\\\}}usr{{/|\\\\}}bin{{/|\\\\}}swift{{c?(\.exe)?}}
// UPDATE-CODE: -frontend -c
// UPDATE-CODE: -emit-remap-file-path {{.+}}.remap

// NO-REFERENCE-DEPENDENCIES: bin{{/|\\\\}}swift
// NO-REFERENCE-DEPENDENCIES-NOT: -emit-reference-dependencies
