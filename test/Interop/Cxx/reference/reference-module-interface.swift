// RUN: %target-swift-ide-test -print-module -module-to-print=Reference -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: func getStaticInt() -> Int32
// CHECK: func getStaticIntRef() -> UnsafeMutablePointer<Int32>
// CHECK: func getStaticIntRvalueRef() -> UnsafeMutablePointer<Int32>
// CHECK: func getConstStaticIntRef() -> UnsafePointer<Int32>
// CHECK: func getConstStaticIntRvalueRef() -> UnsafePointer<Int32>
// CHECK: func setStaticInt(_: Int32)
// CHECK: func setStaticIntRef(_: inout Int32)
// CHECK: func setConstStaticIntRef(_: Int32)
// CHECK: func getFuncRef() -> @convention(c) () -> Int32
// CHECK: func getFuncRvalueRef() -> @convention(c) () -> Int32
// CHECK: func setConstStaticIntRefTypealias(_ ref: Int32)
// CHECK: func setStaticIntRefTypealias(_ ref: inout Int32)
// CHECK: func refToTemplate<T>(_ t: inout T) -> UnsafeMutablePointer<T>
// CHECK: func constRefToTemplate<T>(_ t: T) -> UnsafePointer<T>

// CHECK-NOT: refToDependent
// CHECK-NOT: refToDependentParam
// CHECK-NOT: dontImportAtomicRef
// CHECK-NOT: setStaticIntRvalueRef
// CHECK-NOT: setConstStaticIntRvalueRef
