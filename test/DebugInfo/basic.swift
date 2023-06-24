// A (no longer) basic test for debug info.
// --------------------------------------------------------------------
// Verify that we don't emit any debug info by default.
// RUN: %target-swift-frontend %s -emit-ir -o - \
// RUN:   | %FileCheck %s --check-prefix NDEBUG
// NDEBUG: source_filename
// NDEBUG-NOT: !dbg
// NDEBUG-NOT: DICompileUnit
// --------------------------------------------------------------------
// Verify that we don't emit any debug info with -gnone.
// RUN: %target-swift-frontend %s -emit-ir -gnone -o - \
// RUN:   | %FileCheck %s --check-prefix NDEBUG
// --------------------------------------------------------------------
// Verify that we don't emit any type info with -gline-tables-only.
// RUN: %target-swift-frontend %s -emit-ir -gline-tables-only -o - \
// RUN:   | %FileCheck %s --check-prefix CHECK-LINETABLES
// CHECK: !dbg
// CHECK-LINETABLES-NOT: DI{{.*}}Variable
// CHECK-LINETABLES-NOT: DW_TAG_structure_type
// CHECK-LINETABLES-NOT: DIBasicType
// --------------------------------------------------------------------
// Now check that we do generate line+scope info with -g.
// RUN: %target-swift-frontend %/s -emit-ir -g -o - \
// RUN:   | %FileCheck %s --check-prefixes CHECK,DWARF-CHECK
// --------------------------------------------------------------------
// Currently -gdwarf-types should give the same results as -g.
// RUN: %target-swift-frontend  %/s -emit-ir -gdwarf-types -o - \
// RUN:   | %FileCheck %s --check-prefixes CHECK,DWARF-CHECK
// --------------------------------------------------------------------
// Verify that -g -debug-info-format=dwarf gives the same results as -g.
// RUN: %target-swift-frontend %/s -emit-ir -g -debug-info-format=dwarf -o - \
// RUN:   | %FileCheck %s --check-prefixes CHECK,DWARF-CHECK
// --------------------------------------------------------------------
// RUN: %target-swift-frontend %/s -emit-ir -g -debug-info-format=codeview -o - \
// RUN:   | %FileCheck %s --check-prefixes CHECK,CV-CHECK
// --------------------------------------------------------------------
//
// CHECK: foo
// CHECK-DAG: ret{{.*}}, !dbg ![[RET:[0-9]+]]
// CHECK-DAG: ![[FOO:[0-9]+]] = distinct !DISubprogram(name: "foo",{{.*}} line: [[@LINE+2]],{{.*}} type: ![[FOOTYPE:[0-9]+]]
public
func foo(_ a: Int64, _ b: Int64) -> Int64 {
     var a = a
     var b = b
     // CHECK-DAG: ![[ALOC:.*]] = !DILocation(line: [[@LINE-3]],{{.*}} scope: ![[FOO]])
     // Check that a is the first and b is the second argument.
     // CHECK-DAG: store i64 %0, ptr [[AADDR:.*]], align
     // CHECK-DAG: store i64 %1, ptr [[BADDR:.*]], align
     // CHECK-DAG: [[AVAL:%.*]] = getelementptr inbounds {{.*}}, [[AMEM:.*]], i32 0, i32 0
     // CHECK-DAG: [[BVAL:%.*]] = getelementptr inbounds {{.*}}, [[BMEM:.*]], i32 0, i32 0
     // CHECK-DAG: call void @llvm.dbg.declare(metadata ptr [[AADDR]], metadata ![[AARG:.*]], metadata !DIExpression()), !dbg ![[ALOC]]
     // CHECK-DAG: call void @llvm.dbg.declare(metadata ptr [[BADDR]], metadata ![[BARG:.*]], metadata !DIExpression())
     // CHECK-DAG: ![[AARG]] = !DILocalVariable(name: "a", arg: 1
     // CHECK-DAG: ![[BARG]] = !DILocalVariable(name: "b", arg: 2
     if b != 0 {
       // CHECK-DAG: !DILexicalBlock({{.*}} line: [[@LINE-1]]
       // Transparent inlined multiply:
       // CHECK-DAG: smul{{.*}}, !dbg ![[MUL:[0-9]+]]
       // CHECK-DAG: [[MUL]] = !DILocation(line: [[@LINE+1]],
       return a*b
     } else {
       // CHECK-DAG: ![[PARENT:[0-9]+]] = distinct !DILexicalBlock({{.*}} line: [[@LINE-1]]
       // CHECK-DAG: ![[VARSCOPE:[0-9]+]] = distinct !DILexicalBlock({{.*}} line: [[@LINE+1]]
       var c: Int64 = 42
       // CHECK-DAG: ![[CONDITION:[0-9]+]] = distinct !DILexicalBlock(scope: ![[VARSCOPE]], {{.*}}, line: [[@LINE+1]]
       if a == 0 {
         // CHECK-DAG: !DILexicalBlock(scope: ![[CONDITION]], {{.*}}, line: [[@LINE-1]]
         // What about a nested scope?
         return 0
       }
       return c
     }
}

// CHECK-DAG: ![[MAINFILE:[0-9]+]] = !DIFile(filename: "{{.*}}DebugInfo/basic.swift", directory: "{{.*}}")
// CHECK-DAG: !DICompileUnit(language: DW_LANG_Swift, file: ![[MAINFILE]],{{.*}} producer: "{{.*}}Swift version{{.*}},{{.*}}
// CHECK-DAG: !DISubprogram(name: "main", {{.*}}file: ![[MAINFILE]],

// Function type for foo.
// CHECK-DAG: ![[FOOTYPE]] = !DISubroutineType(types: ![[PARAMTYPES:[0-9]+]])
// CHECK-DAG: ![[INT64:.*]] = !DICompositeType(tag: DW_TAG_structure_type, name: "Int64", {{.*}}, identifier: "$ss5Int64VD")
// CHECK-DAG: ![[PARAMTYPES]] = !{![[INT64]], ![[INT64]], ![[INT64]]}
// Import of the main module with the implicit name.
// CHECK-DAG: !DIImportedEntity(tag: DW_TAG_imported_module, scope: ![[MAINFILE]], entity: ![[MAINMODULE:[0-9]+]], file: ![[MAINFILE]])
// CHECK-DAG: ![[MAINMODULE]] = !DIModule({{.*}}, name: "basic"

// DWARF Version
// DWARF-CHECK-DAG:  i32 7, !"Dwarf Version", i32 4}
// CV-CHECK-DAG: i32 2, !"CodeView", i32 1}

// Debug Info Version
// CHECK-DAG:  i32 2, !"Debug Info Version", i32
