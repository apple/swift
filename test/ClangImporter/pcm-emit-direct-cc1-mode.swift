// Emit the explicit module.
// RUN: %empty-directory(%t)
// RUN: %swift-frontend -emit-pcm -direct-clang-cc1-module-build -only-use-extra-clang-opts -module-name script -o %t/script.pcm %S/Inputs/custom-modules/module.map -Xcc %S/Inputs/custom-modules/module.map -Xcc -o -Xcc %t/script.pcm -Xcc -fmodules -Xcc -triple -Xcc %target-triple -Xcc -x -Xcc objective-c -dump-clang-diagnostics 2> %t.diags.txt

// Verify some of the output of the -dump-pcm flag.
// RUN: %swift-dump-pcm %t/script.pcm | %FileCheck %s --check-prefix=CHECK-DUMP
// CHECK-DUMP: Information for module file '{{.*}}/script.pcm':
// CHECK-DUMP:   Module name: script
// CHECK-DUMP:   Module map file: {{.*[/\\]}}Inputs{{/|\\}}custom-modules{{/|\\}}module.map

// Verify that the clang command-line used is cc1
// RUN: %FileCheck -check-prefix CHECK-CLANG -DTRIPLE=%target-triple %s < %t.diags.txt
// CHECK-CLANG: '{{.*[/\\]}}module.map' '-o' '{{.*[/\\]}}script.pcm' '-fmodules' '-triple' '[[TRIPLE]]' '-x' 'objective-c'

import script
var _ : ScriptTy
