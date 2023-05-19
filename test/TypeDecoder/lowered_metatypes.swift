// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/lowered_metatypes -emit-module
// RUN: sed -ne '/\/\/ *DEMANGLE: /s/\/\/ *DEMANGLE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test %t/lowered_metatypes -type-from-mangled=%t/input | %FileCheck %s --match-full-lines

struct Struct {}
class Class {}
protocol Proto {}

// DEMANGLE: $s17lowered_metatypes6StructVXMt
// DEMANGLE: $s17lowered_metatypes6StructVXMT
// DEMANGLE: $s17lowered_metatypes5ClassCXMo

// CHECK: @thin Struct.Type
// CHECK: @thick Struct.Type
// CHECK: @objc_metatype Class.Type

// DEMANGLE: $s17lowered_metatypes5ProtoPXmT
// DEMANGLE: $s17lowered_metatypes5ProtoPXmo

// CHECK: @thick any Proto.Type
// CHECK: @objc_metatype any Proto.Type
