// RUN: %target-swift-frontend -emit-ir %s -g -I %S/Inputs \
// RUN:   -Xcc -DFOO="foo" -Xcc -UBAR -o - | %FileCheck %s
//
// RUN: %target-swift-frontend -emit-ir %s -g -I %S/Inputs \
// RUN:   -Xcc -DFOO="foo" -Xcc -UBAR -o - -no-clang-module-breadcrumbs \
// RUN:   | %FileCheck %s --check-prefix=NONE
//
// RUN: %target-swift-frontend -module-cache-path %t.mcp -emit-ir %s \
// RUN:   -g -I %S/Inputs -Xcc -DFOO="foo" -Xcc -UBAR \
// RUN:   -debug-prefix-map %t.mcp=PREFIX \
// RUN:   -o - | %FileCheck %s --check-prefix=REMAP

import ClangModule.SubModule
import OtherClangModule.SubModule

let _ = someFunc(0)

// Check for Clang module breadcrumbs.
// CHECK: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}},{{.*}} producer: "{{.*}}Swift{{.*}}ClangModule{{.*}}dwoId:

// CHECK: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}}, {{.*}} producer: "{{.*}}Swift{{.*}}OtherClangModule{{.*}}dwoId:

// CHECK: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}},{{.*}} producer: "{{.*}}clang{{.*}}ClangModule{{.*}}dwoId:

// NONE: DICompileUnit({{.*}}
// NONE-NOT: DICompileUnit({{.*}}ClangModule

// REMAP: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}},{{.*}} producer: "{{.*}}Swift{{.*}}PREFIX{{/|\\\\}}{{.*}}{{/|\\\\}}ClangModule{{.*}}dwoId:

// REMAP: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}},{{.*}} producer: "{{.*}}Swift{{.*}}PREFIX{{/|\\\\}}{{.*}}{{/|\\\\}}OtherClangModule{{.*}}dwoId:

// REMAP: !DICompileUnit(language: DW_LANG_{{ObjC|C99|C11}},{{.*}} producer: "{{.*}}clang{{.*}}PREFIX{{/|\\\\}}{{.*}}{{/|\\\\}}ClangModule{{.*}}dwoId:
