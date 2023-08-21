// RUN: %empty-directory(%t)
// Clang-import a module.
import ClangModule

// Note: This test is highly dependent on the clang module cache
// format, but it is testing specifics of the module cache.

// 1. Test that swift-ide-test creates a clang module with debug info.

// RUN: %empty-directory(%t)
// RUN: %swift-ide-test_plain -print-usrs -target %target-triple -module-cache-path %t  -I %S/Inputs -source-filename %s
// RUN: llvm-readobj -h %t/*/ClangModule-*.pcm | %FileCheck %s --check-prefix CHECK-1
// CHECK-1: Format: {{(Mach-O|ELF|elf64|COFF|elf32-littlearm)}}

// 2. Test that swift is creating clang modules with debug info.

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -c -g -o %t.o -module-cache-path %t -I %S/Inputs
// RUN: llvm-readobj -h %t/*/ClangModule-*.pcm | %FileCheck %s
// CHECK: Format: {{(Mach-O|ELF|elf64|COFF|elf32-littlearm|WASM)}}

// 3. Test that swift-ide-check will not share swiftc's module cache.

// RUN: %swift-ide-test_plain -print-usrs -target %target-triple -module-cache-path %t  -I %S/Inputs -source-filename %s
