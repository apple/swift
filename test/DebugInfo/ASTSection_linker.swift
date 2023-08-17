// Run test ASTSection.swift, with separate compile and link steps.

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -c -Xcc -DA -Xcc -DB -emit-module -o %t %S/ASTSection.swift -swift-version 4
// RUN: %swift-ide-test -test-CompilerInvocation-from-module -source-filename=%t/ASTSection.swiftmodule

// Test the inline section mechanism.
// RUN: %target-ld %t/ASTSection.o -sectcreate __DWARF __swift_ast %t/ASTSection.swiftmodule -o %t/ASTSection.dylib -dylib -L%sdk/usr/lib -lSystem -lobjc
// RUN: %lldb-moduleimport-test -verbose %t/ASTSection.dylib | %FileCheck %s

// Test the symbol table entry.
// RUN: %target-ld %t/ASTSection.o -add_ast_path %t/ASTSection.swiftmodule -o %t/ASTSection.dylib -dylib -L%sdk/usr/lib -lSystem -lobjc
// RUN: %lldb-moduleimport-test -verbose %t/ASTSection.dylib | %FileCheck %s

// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64

// CHECK: - Swift Version: {{.+}}.{{.+}}
// CHECK: - Compatibility Version: 4
// CHECK: - Target: {{.+}}-{{.+}}-{{.+}}
// CHECK: - SDK path: {{.*}}MacOS{{.*}}.sdk
// CHECK: - -Xcc options: -working-directory {{.+}} -DA -DB
// CHECK: Importing ASTSection... ok!
