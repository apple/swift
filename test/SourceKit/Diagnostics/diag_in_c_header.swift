// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %sourcekitd-test -req=diags %t/main.swift -- %t/main.swift -I %t | %FileCheck %s

//--- Header.h
#define FOO(x) #x

//--- module.modulemap
module HeaderWithMacro {
    header "Header.h"
}

//--- main.swift
import HeaderWithMacro

_ = FOO(5)

// rdar://107281079 – Make sure the note points in the .h file
// CHECK:      key.diagnostics: [
// CHECK-NEXT:   {
// CHECK-NEXT:     key.line: 3,
// CHECK-NEXT:     key.column: 5,
// CHECK-NEXT:     key.filepath: "{{.*}}main.swift",
// CHECK-NEXT:     key.severity: source.diagnostic.severity.error,
// CHECK-NEXT:     key.id: "cannot_find_in_scope",
// CHECK-NEXT:     key.description: "cannot find 'FOO' in scope",
// CHECK-NEXT:     key.ranges: [
// CHECK-NEXT:       {
// CHECK-NEXT:         key.offset: 28,
// CHECK-NEXT:         key.length: 3
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     key.diagnostics: [
// CHECK-NEXT:       {
// CHECK-NEXT:         key.line: 1,
// CHECK-NEXT:         key.column: 9,
// CHECK-NEXT:         key.filepath: "{{.*}}Header.h",
// CHECK-NEXT:         key.severity: source.diagnostic.severity.note,
// CHECK-NEXT:         key.id: "macro_not_imported_function_like",
// CHECK-NEXT:         key.description: "macro 'FOO' unavailable: function like macros not supported"
// CHECK-NEXT:       }
// CHECK-NEXT:     ]
// CHECK-NEXT:   }
// CHECK-NEXT: ]
