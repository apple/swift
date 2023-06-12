// RUN: %target-swift-ide-test -print-module -module-to-print=MutableMembers -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop -Xcc -DUSE_MUTATING -I %swift_src_root/lib/ClangImporter | %FileCheck %s

// CHECK: struct HasPublicMutableMember {
// CHECK:   mutating func foo() -> Int32
// CHECK:   var a: Int32
// CHECK: }

// CHECK: struct HasPrivateMutableMember {
// CHECK:   mutating func bar()
// CHECK: }
