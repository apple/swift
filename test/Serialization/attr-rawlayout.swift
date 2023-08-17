// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -emit-module-path %t/a.swiftmodule -module-name a %s
// RUN: llvm-bcanalyzer -dump %t/a.swiftmodule | %FileCheck --check-prefix BC-CHECK --implicit-check-not UnknownCode %s
// RUN: %target-swift-ide-test -print-module -module-to-print a -source-filename x -I %t | %FileCheck --check-prefix MODULE-CHECK %s

// BC-CHECK: <RawLayout_DECL_ATTR 

// MODULE-CHECK: @_rawLayout(size: 5, alignment: 4) struct A_ExplicitSizeAlign
@_rawLayout(size: 5, alignment: 4)
struct A_ExplicitSizeAlign: ~Copyable {}

// MODULE-CHECK: @_rawLayout(like: T) struct B_Cell
@_rawLayout(like: T)
struct B_Cell<T>: ~Copyable {}

// MODULE-CHECK: @_rawLayout(likeArrayOf: T, count: 8) struct C_SmallVector
@_rawLayout(likeArrayOf: T, count: 8)
struct C_SmallVector<T>: ~Copyable {}
