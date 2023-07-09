// RUN: %target-swift-ide-test -print-module -module-to-print=ClassTemplateNonTypeParameter -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: struct MagicArray<T, Size> {
// CHECK: }

// CHECK: struct MagicArray<Int32, _UInt{{.*}}_2> {
// CHECK:   init()
// CHECK:   init(t: (Int32, Int32))
// CHECK:   var t: (Int32, Int32)
// CHECK: }

// CHECK: struct MagicArray<Int32, _UInt{{.*}}_3> {
// CHECK:   init()
// CHECK:   init(t: (Int32, Int32, Int32))
// CHECK:   var t: (Int32, Int32, Int32)
// CHECK: }

// CHECK: typealias MagicIntPair = MagicArray<Int32, _UInt{{.*}}_2>
// CHECK: typealias MagicIntTriple = MagicArray<Int32, _UInt{{.*}}_3>
